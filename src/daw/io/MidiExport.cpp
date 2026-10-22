#include "daw/io/MidiExport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace daw {

namespace {

constexpr int kPpq = 480;
constexpr double kRampSteps = 32.0; // a ramp between two points is sampled every 1/32 beat
constexpr double kEpsilon = 1e-9;

using Bytes = std::vector<uint8_t>;

// Ordering at one tick: note offs first, then controllers, then note ons, so a note ends before
// the next one on its key starts
constexpr int kOrderOff = 0;
constexpr int kOrderControl = 1;
constexpr int kOrderOn = 2;

struct Event {
    long long tick = 0;
    int order = 0;
    size_t seq = 0; // equal events keep the order they were made in
    Bytes bytes;
};

// A controller value at a time: a controller change (7-bit) or a pitch bend (14-bit). `point` values
// are always written; sampled values only when they differ from the last one written.
struct ControlValue {
    long long tick = 0;
    int controller = 0; // 0-127, or kPitchBend
    int value = 0;
    bool point = false;
    size_t seq = 0;
};

// The channel of the Nth exported track: 0-8 and 10-15 in turn, with the drum channel first when
// it is asked for
int channelFor(size_t ordinal, bool drumChannel)
{
    static constexpr std::array<int, 15> kPlain = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
    if (drumChannel) {
        if (ordinal == 0)
            return 9;
        --ordinal;
    }
    return kPlain[ordinal % kPlain.size()];
}

void putVlq(Bytes &out, uint32_t value)
{
    std::array<uint8_t, 5> groups{};
    int count = 0;
    do {
        groups[static_cast<size_t>(count++)] = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
    } while (value != 0);
    for (int i = count - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>(groups[static_cast<size_t>(i)] | (i > 0 ? 0x80 : 0)));
}

void putU16(Bytes &out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void putU32(Bytes &out, uint32_t value)
{
    putU16(out, value >> 16);
    putU16(out, value & 0xFFFF);
}

// A meta event: FF, its type, a length and its data
void putMeta(Bytes &out, uint8_t type, const Bytes &data)
{
    out.push_back(0xFF);
    out.push_back(type);
    putVlq(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

int clampInt(long value, int low, int high)
{
    return static_cast<int>(std::clamp(value, static_cast<long>(low), static_cast<long>(high)));
}

// The file's tick for a beat on the timeline, with time 0 at the export's start
long long tickAt(double beat, double from)
{
    return std::llround((beat - from) * kPpq);
}

// The 7-bit value of a 0-1 controller value, and the 14-bit value of a pitch bend (0.5 is centre)
int controlValue7(float value) { return clampInt(std::lround(static_cast<double>(value) * 127.0), 0, 127); }
int controlValue14(float value) { return clampInt(std::lround(static_cast<double>(value) * 16384.0), 0, 16383); }

// The note events of one track's clips in the range, on `channel`
void addNotes(const Track &track, double from, double to, int channel, std::vector<Event> &events, size_t &seq)
{
    for (const Clip &clip : track.clips) {
        const double clipEnd = clip.start + clip.length;
        for (const Note &note : clip.notes) {
            if (note.muted || note.length <= 0.0)
                continue;
            const double start = clip.start + note.start;
            // Notes past the clip's end don't play; those that run over it are cut at it, and at the range's end
            if (start < from - kEpsilon || start >= clipEnd - kEpsilon || start >= to - kEpsilon)
                continue;
            const double end = std::min({start + note.length, clipEnd, to});
            if (end <= start + kEpsilon)
                continue;
            const long long on = tickAt(start, from);
            const long long off = std::max(tickAt(end, from), on + 1);
            const int key = clampInt(note.key, 0, 127);
            const int velocity = clampInt(std::lround(static_cast<double>(note.velocity) * 127.0), 1, 127);
            events.push_back({on, kOrderOn, seq++, {static_cast<uint8_t>(0x90 | channel), static_cast<uint8_t>(key), static_cast<uint8_t>(velocity)}});
            events.push_back({off, kOrderOff, seq++, {static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(key), 64}});
        }
    }
}

// The controller values of one track's lanes in the range, in time order. Each point is kept; a
// ramp to the next point is sampled every 1/32 beat.
std::vector<ControlValue> controlValues(const Track &track, double from, double to)
{
    std::vector<ControlValue> values;
    size_t seq = 0;
    for (const Clip &clip : track.clips) {
        const double clipEnd = clip.start + clip.length;
        for (const ControlLane &lane : clip.controls) {
            const bool bend = lane.controller == kPitchBend;
            const auto valueOf = [&](float value) { return bend ? controlValue14(value) : controlValue7(value); };
            const auto inRange = [&](double beat) { return beat >= from - kEpsilon && beat < to - kEpsilon && beat < clipEnd - kEpsilon; };
            const size_t count = lane.points.size();
            for (size_t i = 0; i < count; ++i) {
                const ControlPoint &point = lane.points[i];
                const double beat = clip.start + point.beat;
                if (inRange(beat))
                    values.push_back({tickAt(beat, from), lane.controller, valueOf(point.value), true, seq++});
                if (i + 1 >= count || controllerStepped(lane.controller))
                    continue;
                const ControlPoint &next = lane.points[i + 1];
                if (next.beat <= point.beat)
                    continue;
                for (int step = 1;; ++step) {
                    const double local = point.beat + static_cast<double>(step) / kRampSteps;
                    if (local >= next.beat - kEpsilon)
                        break;
                    const double beat = clip.start + local;
                    if (!inRange(beat))
                        continue;
                    values.push_back({tickAt(beat, from), lane.controller, valueOf(lane.valueAt(local)), false, seq++});
                }
            }
        }
    }
    std::stable_sort(values.begin(), values.end(), [](const ControlValue &a, const ControlValue &b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.point && !b.point;
    });
    // Drops sampled values that repeat the last one written for their controller
    std::array<int, kPitchBend + 1> last{};
    last.fill(-1);
    std::vector<ControlValue> written;
    for (const ControlValue &v : values) {
        if (!v.point && last[static_cast<size_t>(v.controller)] == v.value)
            continue;
        last[static_cast<size_t>(v.controller)] = v.value;
        written.push_back(v);
    }
    return written;
}

// Every event of an instrument track in the range, on `channel`, with its name first
std::vector<Event> trackEvents(const Track &track, double from, double to, int channel)
{
    std::vector<Event> events;
    size_t seq = 0;
    addNotes(track, from, to, channel, events, seq);
    for (const ControlValue &v : controlValues(track, from, to)) {
        Bytes bytes;
        if (v.controller == kPitchBend)
            bytes = {static_cast<uint8_t>(0xE0 | channel), static_cast<uint8_t>(v.value & 0x7F), static_cast<uint8_t>((v.value >> 7) & 0x7F)};
        else
            bytes = {static_cast<uint8_t>(0xB0 | channel), static_cast<uint8_t>(v.controller), static_cast<uint8_t>(v.value)};
        events.push_back({v.tick, kOrderControl, seq++, bytes});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        if (a.order != b.order)
            return a.order < b.order;
        return a.seq < b.seq;
    });
    return events;
}

// One MTrk chunk: the events with their delta times, then the end of track
Bytes trackChunk(const std::string &name, const std::vector<Event> &events)
{
    Bytes body;
    Bytes nameData(name.begin(), name.end());
    putVlq(body, 0);
    putMeta(body, 0x03, nameData);
    long long previous = 0;
    for (const Event &event : events) {
        putVlq(body, static_cast<uint32_t>(event.tick - previous));
        body.insert(body.end(), event.bytes.begin(), event.bytes.end());
        previous = event.tick;
    }
    putVlq(body, 0);
    putMeta(body, 0x2F, {});
    Bytes chunk = {'M', 'T', 'r', 'k'};
    putU32(chunk, static_cast<uint32_t>(body.size()));
    chunk.insert(chunk.end(), body.begin(), body.end());
    return chunk;
}

}

bool writeMidiFile(const Project &project, const MidiExportOptions &options, const std::string &path, std::string &error)
{
    // The range: from fromBeat, to toBeat, or to the end of the last clip in the project
    const double from = std::max(0.0, options.fromBeat);
    double to = options.toBeat;
    if (to < 0.0) {
        to = from;
        for (const Track &track : project.tracks)
            for (const Clip &clip : track.clips)
                to = std::max(to, clip.start + clip.length);
    }

    // The tracks written: those asked for (instrument tracks only), else every instrument track
    // with something in the range
    std::vector<int> chosen;
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &track = project.tracks[i];
        if (track.kind != TrackKind::Instrument)
            continue;
        const int index = static_cast<int>(i);
        const bool asked = !options.tracks.empty() && std::find(options.tracks.begin(), options.tracks.end(), index) != options.tracks.end();
        if (options.tracks.empty() ? !trackEvents(track, from, to, 0).empty() : asked)
            chosen.push_back(index);
    }

    // Track 0 is the conductor: the name, tempo and time signature
    const int beatsPerBar = std::clamp(project.beatsPerBar, 1, 255);
    const double tempo = project.tempo > 0.0 ? project.tempo : 120.0;
    Bytes conductor;
    {
        Bytes name(project.name.begin(), project.name.end());
        const auto microseconds = static_cast<uint32_t>(std::llround(60e6 / tempo));
        const Bytes tempoData = {static_cast<uint8_t>(microseconds >> 16), static_cast<uint8_t>(microseconds >> 8), static_cast<uint8_t>(microseconds)};
        // Time signature: beats per bar over a quarter note, 24 clocks a click, 8 notated 32nds a quarter
        const Bytes signature = {static_cast<uint8_t>(beatsPerBar), 2, 24, 8};
        putVlq(conductor, 0);
        putMeta(conductor, 0x03, name);
        putVlq(conductor, 0);
        putMeta(conductor, 0x51, tempoData);
        putVlq(conductor, 0);
        putMeta(conductor, 0x58, signature);
        putVlq(conductor, 0);
        putMeta(conductor, 0x2F, {});
    }
    Bytes conductorChunk = {'M', 'T', 'r', 'k'};
    putU32(conductorChunk, static_cast<uint32_t>(conductor.size()));
    conductorChunk.insert(conductorChunk.end(), conductor.begin(), conductor.end());

    // The file: header, conductor, then one track per chosen instrument track on its channel
    Bytes file = {'M', 'T', 'h', 'd'};
    putU32(file, 6);
    putU16(file, 1);
    putU16(file, static_cast<uint32_t>(chosen.size() + 1));
    putU16(file, kPpq);
    file.insert(file.end(), conductorChunk.begin(), conductorChunk.end());
    for (size_t ordinal = 0; ordinal < chosen.size(); ++ordinal) {
        const Track &track = project.tracks[static_cast<size_t>(chosen[ordinal])];
        const int channel = channelFor(ordinal, options.drumChannel);
        std::vector<Event> events = trackEvents(track, from, to, channel);
        Bytes chunk = trackChunk(track.name, events);
        file.insert(file.end(), chunk.begin(), chunk.end());
    }

    // Written beside the file and renamed, so a failed write never leaves a half file behind
    const std::string part = path + ".part";
    namespace fs = std::filesystem;
    {
        std::ofstream out(fs::u8path(part), std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Couldn't create " + path;
            return false;
        }
        out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(fs::u8path(part), ec);
            error = "Couldn't write " + path;
            return false;
        }
    }
    std::error_code ec;
    fs::remove(fs::u8path(path), ec); // rename alone won't replace a file on Windows
    fs::rename(fs::u8path(part), fs::u8path(path), ec);
    if (ec) {
        std::error_code removeEc;
        fs::remove(fs::u8path(part), removeEc);
        error = "Couldn't save " + path;
        return false;
    }
    return true;
}

}
