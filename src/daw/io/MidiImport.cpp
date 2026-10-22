#include "daw/io/MidiImport.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace daw {

namespace {

constexpr size_t kMaxFileBytes = size_t(64) << 20; // larger than any song needs

// Reads bytes in order, never past the end; a short read sets `failed`
struct Bytes {
    const std::vector<uint8_t> &data;
    size_t at = 0;
    size_t end = 0;
    bool failed = false;

    bool more() const { return at < end; }
    uint8_t u8()
    {
        if (at >= end) {
            failed = true;
            return 0;
        }
        return data[at++];
    }
    uint32_t u16() { return (static_cast<uint32_t>(u8()) << 8) | u8(); }
    uint32_t u32() { return (u16() << 16) | u16(); }
    // A variable-length quantity: at most four bytes
    uint32_t vlq()
    {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t b = u8();
            value = (value << 7) | (b & 0x7F);
            if (!(b & 0x80))
                return value;
        }
        failed = true;
        return value;
    }
    void skip(size_t n)
    {
        if (n > end - at) {
            failed = true;
            at = end;
        } else {
            at += n;
        }
    }
};

// One track's events, read from its chunk
struct TrackEvents {
    std::string name;
    struct NoteOn {
        uint64_t tick;
        float velocity;
    };
    struct Event {
        uint64_t tick;
        int channel;
        int controller; // 0-127, or kPitchBend
        float value;
    };
    struct Played {
        uint64_t start, end;
        int channel, key;
        float velocity;
    };
    std::vector<Played> notes;
    std::vector<Event> controls;
    std::map<int, int> programs; // channel to its first program
    uint64_t endTick = 0;
};

bool readTrack(Bytes &in, TrackEvents &track, MidiFile &file, std::string &error)
{
    std::map<std::pair<int, int>, std::deque<TrackEvents::NoteOn>> held; // (channel, key) to its notes on, oldest first
    uint64_t tick = 0;
    uint8_t status = 0;
    while (in.more() && !in.failed) {
        tick += in.vlq();
        uint8_t b = in.u8();
        if (b == 0xFF) { // meta
            const uint8_t type = in.u8();
            const uint32_t length = in.vlq();
            if (in.failed || length > in.end - in.at)
                break;
            const size_t start = in.at;
            if (type == 0x03 && track.name.empty())
                track.name.assign(in.data.begin() + static_cast<std::ptrdiff_t>(start), in.data.begin() + static_cast<std::ptrdiff_t>(start + length));
            else if (type == 0x51 && length == 3) {
                const uint32_t micros = (static_cast<uint32_t>(in.data[start]) << 16) | (static_cast<uint32_t>(in.data[start + 1]) << 8) | in.data[start + 2];
                if (micros > 0)
                    file.tempo.push_back({static_cast<double>(tick) / file.ticksPerQuarter, 60e6 / micros, false});
            } else if (type == 0x58 && length >= 2) {
                const int numerator = in.data[start];
                const int power = std::min<int>(in.data[start + 1], 6);
                file.signatures.push_back({static_cast<double>(tick) / file.ticksPerQuarter, std::max(1, numerator), 1 << power});
            } else if (type == 0x2F) {
                in.skip(length);
                break;
            }
            in.skip(length);
            continue;
        }
        if (b == 0xF0 || b == 0xF7) { // system exclusive: skipped; it ends running status
            in.skip(in.vlq());
            status = 0;
            continue;
        }
        uint8_t first = 0;
        if (b & 0x80) {
            status = b;
            first = in.u8();
        } else {
            if (!status) {
                error = "a track's data begins without a status byte";
                return false;
            }
            first = b; // running status: this is the first data byte
        }
        const int kind = status & 0xF0, channel = status & 0x0F;
        const bool oneByte = kind == 0xC0 || kind == 0xD0;
        const uint8_t second = oneByte ? 0 : in.u8();
        if (in.failed)
            break;
        const int key = first & 0x7F, value = second & 0x7F;
        if (kind == 0x90 && value > 0) {
            held[{channel, key}].push_back({tick, value / 127.0f});
        } else if (kind == 0x80 || kind == 0x90) {
            auto &queue = held[{channel, key}];
            if (!queue.empty()) {
                track.notes.push_back({queue.front().tick, tick, channel, key, queue.front().velocity});
                queue.pop_front();
            }
        } else if (kind == 0xB0 && key < 120) {
            track.controls.push_back({tick, channel, key, value / 127.0f});
        } else if (kind == 0xE0) {
            track.controls.push_back({tick, channel, kPitchBend, static_cast<float>(key | (value << 7)) / 16383.0f});
        } else if (kind == 0xC0) {
            track.programs.emplace(channel, key);
        }
    }
    if (in.failed) {
        error = "a track runs past the end of the file";
        return false;
    }
    // Notes still held when the track ended end there
    for (auto &[where, queue] : held) {
        for (const TrackEvents::NoteOn &on : queue)
            track.notes.push_back({on.tick, std::max(tick, on.tick + 1), where.first, where.second, on.velocity});
    }
    track.endTick = tick;
    return true;
}

// The events of one channel (or all, -1) of a track as a part
MidiPart partOf(const TrackEvents &track, int channel, int ticksPerQuarter)
{
    MidiPart part;
    part.name = track.name;
    const double tpq = static_cast<double>(ticksPerQuarter);
    bool first = true;
    for (const TrackEvents::Played &n : track.notes) {
        if (channel >= 0 && n.channel != channel)
            continue;
        if (first) {
            part.channel = n.channel;
            first = false;
        }
        Note note;
        note.start = static_cast<double>(n.start) / tpq;
        note.length = std::max(1.0 / 64.0, static_cast<double>(n.end - n.start) / tpq);
        note.key = n.key;
        note.velocity = n.velocity;
        part.notes.push_back(note);
    }
    std::stable_sort(part.notes.begin(), part.notes.end(), [](const Note &a, const Note &b) { return a.start < b.start; });
    for (const TrackEvents::Event &e : track.controls) {
        if (channel >= 0 && e.channel != channel)
            continue;
        if (first) {
            part.channel = e.channel;
            first = false;
        }
        auto lane = std::find_if(part.controls.begin(), part.controls.end(), [&e](const ControlLane &l) { return l.controller == e.controller; });
        if (lane == part.controls.end()) {
            part.controls.push_back({e.controller, {}});
            lane = part.controls.end() - 1;
        }
        lane->points.push_back({static_cast<double>(e.tick) / tpq, e.value});
    }
    if (const auto program = track.programs.find(part.channel); program != track.programs.end())
        part.program = program->second;
    return part;
}

}

double MidiPart::end() const
{
    double end = 0.0;
    for (const Note &n : notes)
        end = std::max(end, n.start + n.length);
    for (const ControlLane &lane : controls) {
        if (!lane.points.empty())
            end = std::max(end, lane.points.back().beat);
    }
    return end;
}

size_t MidiFile::noteCount() const
{
    size_t n = 0;
    for (const MidiPart &p : parts)
        n += p.notes.size();
    return n;
}

bool readMidi(const std::vector<uint8_t> &bytes, MidiFile &out, std::string &error)
{
    out = MidiFile();
    Bytes in{bytes, 0, bytes.size()};
    if (bytes.size() < 14 || bytes[0] != 'M' || bytes[1] != 'T' || bytes[2] != 'h' || bytes[3] != 'd') {
        error = "it isn't a MIDI file";
        return false;
    }
    in.at = 4;
    const uint32_t headerLength = in.u32();
    out.format = static_cast<int>(in.u16());
    const uint32_t tracks = in.u16();
    const uint32_t division = in.u16();
    if (headerLength < 6) {
        error = "its header is too short";
        return false;
    }
    in.at = 8 + headerLength;
    if (out.format > 1) {
        error = "it is a type " + std::to_string(out.format) + " file (types 0 and 1 are read)";
        return false;
    }
    if (division & 0x8000) {
        error = "it counts time in SMPTE frames rather than beats";
        return false;
    }
    if (division == 0) {
        error = "it has no ticks per beat";
        return false;
    }
    out.ticksPerQuarter = static_cast<int>(division);

    std::vector<TrackEvents> read;
    for (uint32_t t = 0; t < tracks && in.at + 8 <= bytes.size();) {
        const bool isTrack = bytes[in.at] == 'M' && bytes[in.at + 1] == 'T' && bytes[in.at + 2] == 'r' && bytes[in.at + 3] == 'k';
        in.at += 4;
        const uint32_t length = in.u32();
        if (length > bytes.size() - in.at) {
            error = "a track is longer than the file";
            return false;
        }
        if (!isTrack) { // a chunk of some other kind: skipped
            in.at += length;
            continue;
        }
        Bytes chunk{bytes, in.at, in.at + length};
        TrackEvents events;
        if (!readTrack(chunk, events, out, error)) {
            error = "track " + std::to_string(t + 1) + ": " + error;
            return false;
        }
        read.push_back(std::move(events));
        in.at += length;
        ++t;
    }
    if (read.empty()) {
        error = "it has no tracks";
        return false;
    }

    // The parts: a type 0 file's track split by channel; a type 1 file's tracks with something in them
    if (out.format == 1)
        out.name = read.front().name;
    for (const TrackEvents &track : read) {
        if (track.notes.empty() && track.controls.empty())
            continue;
        if (out.format == 0) {
            std::vector<int> channels;
            for (const auto &n : track.notes)
                channels.push_back(n.channel);
            for (const auto &e : track.controls)
                channels.push_back(e.channel);
            std::sort(channels.begin(), channels.end());
            channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
            for (int channel : channels) {
                MidiPart part = partOf(track, channel, out.ticksPerQuarter);
                if (channels.size() > 1 || part.name.empty())
                    part.name = (part.name.empty() ? std::string() : part.name + " ") + (channel == 9 ? "Drums" : "Channel " + std::to_string(channel + 1));
                if (!part.notes.empty() || !part.controls.empty())
                    out.parts.push_back(std::move(part));
            }
        } else {
            out.parts.push_back(partOf(track, -1, out.ticksPerQuarter));
        }
    }
    for (MidiPart &part : out.parts) {
        for (ControlLane &lane : part.controls)
            std::stable_sort(lane.points.begin(), lane.points.end(), [](const ControlPoint &a, const ControlPoint &b) { return a.beat < b.beat; });
    }
    std::stable_sort(out.tempo.begin(), out.tempo.end(), [](const audio::TempoPoint &a, const audio::TempoPoint &b) { return a.beat < b.beat; });
    std::stable_sort(out.signatures.begin(), out.signatures.end(), [](const MidiFile::Meter &a, const MidiFile::Meter &b) { return a.beat < b.beat; });
    return true;
}

bool readMidiFile(const std::string &path, MidiFile &out, std::string &error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = fs::u8path(path);
    const auto size = fs::file_size(file, ec);
    if (ec) {
        error = "Couldn't read " + path;
        return false;
    }
    if (size > kMaxFileBytes) {
        error = file.filename().u8string() + " is too large for a MIDI file";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!readMidi(bytes, out, error)) {
        error = file.filename().u8string() + ": " + error;
        return false;
    }
    return true;
}

int importMidi(Project &project, const MidiFile &file, double at, int track, const audio::DeviceSetting &instrument,
               const std::string &fallback)
{
    if (file.parts.empty())
        return -1;
    const audio::TempoMap &timing = project.timing;
    int first = -1;
    int insertAt = track >= 0 && static_cast<size_t>(track) < project.tracks.size() ? track + 1 : static_cast<int>(project.tracks.size());
    double latest = at;
    for (size_t i = 0; i < file.parts.size(); ++i) {
        const MidiPart &part = file.parts[i];
        // To the bar line at or after its end, at least a bar
        const double end = at + part.end();
        const double barEnd = timing.barStart(timing.barAt(end - 1e-9) + 1);
        const double length = std::max(barEnd, timing.barStart(timing.barAt(at) + 1)) - at;
        const std::string name = part.name.empty() ? fallback + (file.parts.size() > 1 ? " " + std::to_string(i + 1) : std::string()) : part.name;
        Clip clip(name, at, length, part.notes);
        clip.controls = part.controls;
        latest = std::max(latest, at + length);
        int index = -1;
        if (i == 0 && track >= 0 && static_cast<size_t>(track) < project.tracks.size() &&
            project.tracks[static_cast<size_t>(track)].kind == TrackKind::Instrument) {
            index = track;
        } else {
            Track fresh;
            fresh.name = name;
            fresh.kind = TrackKind::Instrument;
            fresh.instrument = instrument;
            project.tracks.insert(project.tracks.begin() + insertAt, std::move(fresh));
            index = insertAt++;
            if (track >= index)
                ++track;
        }
        project.tracks[static_cast<size_t>(index)].clips.push_back(std::move(clip));
        if (first < 0)
            first = index;
    }
    if (latest > project.lengthBeats())
        project.bars = timing.barAt(latest - 1e-9) + 2;
    return first;
}

audio::TempoMap fileTiming(const audio::TempoMap &project, const MidiFile &file, double at)
{
    audio::TempoMap timing = project;
    if (!file.tempo.empty()) {
        std::vector<audio::TempoPoint> points;
        for (const audio::TempoPoint &p : project.points()) {
            if (p.beat < at - 1e-9)
                points.push_back(p);
        }
        // The project's tempo holds up to `at`; the file's first tempo (or its later ones) from there
        if (!points.empty() && points.back().ramp)
            points.back().ramp = false;
        for (audio::TempoPoint p : file.tempo) {
            p.beat += at;
            points.push_back(p);
        }
        timing.setPoints(std::move(points));
    }
    if (!file.signatures.empty()) {
        // Each of the file's signatures at its bar, counted in the file's own bars from `at`'s bar
        const int base = project.barAt(at);
        std::vector<audio::Signature> signatures;
        for (const audio::Signature &s : project.signatures()) {
            if (s.bar < base)
                signatures.push_back(s);
        }
        double beat = 0.0, barLength = 4.0;
        int bar = 0;
        for (const MidiFile::Meter &m : file.signatures) {
            bar += static_cast<int>(std::lround((m.beat - beat) / barLength));
            beat = m.beat;
            const audio::Signature s{base + bar, m.numerator, m.denominator};
            barLength = s.barLength();
            signatures.push_back(s);
        }
        timing.setSignatures(std::move(signatures));
    }
    return timing;
}

}
