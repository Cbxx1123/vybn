#include "MidiEffectsSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "audio/AudioAnalysis.h"
#include "audio/DeviceChain.h"
#include "audio/MidiEffects.h"
#include "audio/Synth.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 512;

// What an effect makes of some events over `frames` frames at 120 BPM from beat 0, a block at a time;
// each event's frame made absolute
std::vector<audio::MidiEvent> runEffect(audio::MidiEffect &effect, std::vector<audio::MidiEvent> events, uint64_t frames)
{
    std::vector<audio::MidiEvent> out;
    for (uint64_t start = 0; start < frames; start += kBlock) { // (events past `frames` are left out)
        audio::MidiEventList in, made;
        for (const audio::MidiEvent &e : events) {
            if (e.frame >= start && e.frame < start + kBlock) {
                audio::MidiEvent local = e;
                local.frame = static_cast<uint32_t>(e.frame - start);
                in.push(local);
            }
        }
        in.sort();
        audio::MidiClock clock;
        clock.frames = kBlock;
        clock.sampleRate = kRate;
        clock.start = start;
        clock.beatsPerFrame = 2.0 / kRate; // 120 BPM
        clock.beat0 = static_cast<double>(start) * clock.beatsPerFrame;
        effect.run(in, made, clock);
        made.sort();
        for (size_t i = 0; i < made.count; ++i) {
            audio::MidiEvent e = made.events[i];
            e.frame += static_cast<uint32_t>(start);
            if (e.frame < frames)
                out.push_back(e);
        }
    }
    return out;
}

audio::DeviceSetting with(audio::MidiEffectType type, std::vector<std::pair<uint32_t, double>> values)
{
    audio::DeviceSetting s = audio::midiEffectSetting(type);
    for (const auto &[id, value] : values)
        s.setValue(id, value);
    return s;
}

std::vector<audio::MidiEvent> ons(const std::vector<audio::MidiEvent> &events)
{
    std::vector<audio::MidiEvent> out;
    for (const audio::MidiEvent &e : events)
        if (e.on)
            out.push_back(e);
    return out;
}

std::string keysOf(const std::vector<audio::MidiEvent> &events)
{
    std::string s;
    for (const audio::MidiEvent &e : events)
        s += (s.empty() ? "" : " ") + std::to_string(e.key);
    return s;
}

}

int runMidiEffectAudioChecks()
{
    Checker c;
    const std::vector<audio::MidiEvent> chord{{0, true, 60, 0.8f}, {0, true, 64, 0.8f}, {0, true, 67, 0.8f}};

    // The arpeggiator up at 1/16: a note every 6000 frames (a quarter beat at 120), each half a step long
    {
        auto arp = audio::makeMidiEffect(audio::midiEffectSetting(audio::MidiEffectType::Arpeggiator));
        const std::vector<audio::MidiEvent> out = runEffect(*arp, chord, 24000);
        const std::vector<audio::MidiEvent> played = ons(out);
        bool timed = played.size() == 4;
        for (size_t i = 0; timed && i < played.size(); ++i)
            timed = played[i].frame == i * 6000;
        bool gated = false;
        for (const audio::MidiEvent &e : out)
            gated = gated || (!e.on && e.key == 60 && e.frame == 3000);
        c.check(timed && gated && keysOf(played) == "60 64 67 60", "MIDI effects: the arpeggiator steps up on the grid, gated",
                keysOf(played));
    }

    // Down over two octaves, and Up-down without repeating its ends
    {
        auto down = audio::makeMidiEffect(with(audio::MidiEffectType::Arpeggiator, {{0, 1}, {2, 2}}));
        const std::string d = keysOf(ons(runEffect(*down, chord, 36000)));
        auto updown = audio::makeMidiEffect(with(audio::MidiEffectType::Arpeggiator, {{0, 2}}));
        const std::string u = keysOf(ons(runEffect(*updown, chord, 36000)));
        c.check(d == "79 76 72 67 64 60" && u == "60 64 67 64 60 64", "MIDI effects: the arpeggiator's down, octaves and up-down",
                d + " / " + u);
    }

    // Hold: the chord plays on after its keys are let go, and a new press starts a new one
    {
        auto arp = audio::makeMidiEffect(with(audio::MidiEffectType::Arpeggiator, {{7, 1}}));
        std::vector<audio::MidiEvent> events = chord;
        events.push_back({1000, false, 60, 0.0f});
        events.push_back({1000, false, 64, 0.0f});
        events.push_back({1000, false, 67, 0.0f});
        events.push_back({20000, true, 50, 0.8f});
        const std::string keys = keysOf(ons(runEffect(*arp, events, 36000)));
        c.check(keys == "60 64 67 60 50 50", "MIDI effects: the arpeggiator's hold latches until a new press", keys);
    }

    // A major chord on each key, its added notes counted, so overlapping chords end cleanly
    {
        auto chordFx = audio::makeMidiEffect(audio::midiEffectSetting(audio::MidiEffectType::Chord));
        const std::vector<audio::MidiEvent> out =
            runEffect(*chordFx, {{0, true, 60, 0.8f}, {100, true, 64, 0.8f}, {200, false, 60, 0.0f}, {300, false, 64, 0.0f}}, 1024);
        std::string offsAt200, offsAt300;
        for (const audio::MidiEvent &e : out) {
            if (!e.on && e.frame == 200)
                offsAt200 += std::to_string(e.key) + " ";
            if (!e.on && e.frame == 300)
                offsAt300 += std::to_string(e.key) + " ";
        }
        const std::string first = keysOf(std::vector<audio::MidiEvent>(out.begin(), out.begin() + 3));
        c.check(first == "60 64 67" && offsAt200 == "60 67 " && offsAt300 == "64 68 71 ",
                "MIDI effects: a chord's notes, and overlapping chords' clean ends", offsAt200 + "/ " + offsAt300);
    }

    // C major: C# goes to D (nearest, up first), or C going down; its note-off follows
    {
        auto nearest = audio::makeMidiEffect(audio::midiEffectSetting(audio::MidiEffectType::Scale));
        const std::vector<audio::MidiEvent> a = runEffect(*nearest, {{0, true, 61, 0.8f}, {10, false, 61, 0.0f}}, 512);
        auto down = audio::makeMidiEffect(with(audio::MidiEffectType::Scale, {{2, 2}}));
        const std::vector<audio::MidiEvent> b = runEffect(*down, {{0, true, 61, 0.8f}, {10, false, 61, 0.0f}}, 512);
        auto minor = audio::makeMidiEffect(with(audio::MidiEffectType::Scale, {{0, 9}, {1, 1}})); // A minor
        const std::vector<audio::MidiEvent> m = runEffect(*minor, {{0, true, 68, 0.8f}}, 512);
        c.check(a.size() == 2 && a[0].key == 62 && a[1].key == 62 && !a[1].on && b.size() == 2 && b[0].key == 60 && m.size() == 1 &&
                    m[0].key == 69,
                "MIDI effects: the scale moves notes into it, their note-offs following");
    }

    // Echoes: each an eighth later (12000 frames), quieter by the feedback, an octave up each time
    {
        auto echo = audio::makeMidiEffect(with(audio::MidiEffectType::Echo, {{1, 2}, {2, 50}, {3, 12}}));
        const std::vector<audio::MidiEvent> out = runEffect(*echo, {{0, true, 60, 0.8f}, {1000, false, 60, 0.0f}}, 30000);
        const std::vector<audio::MidiEvent> played = ons(out);
        bool offs = false;
        for (const audio::MidiEvent &e : out)
            offs = offs || (!e.on && e.key == 84 && e.frame == 25000);
        c.check(played.size() == 3 && played[1].frame == 12000 && played[1].key == 72 && std::fabs(played[1].velocity - 0.4f) < 1e-6f &&
                    played[2].frame == 24000 && played[2].key == 84 && std::fabs(played[2].velocity - 0.2f) < 1e-6f && offs,
                "MIDI effects: echoes' times, levels, pitches and note-offs", keysOf(played));
    }

    // Velocity: into its range, the drive lifting soft notes
    {
        auto velocity = audio::makeMidiEffect(with(audio::MidiEffectType::Velocity, {{1, 64}, {2, 127}}));
        const std::vector<audio::MidiEvent> out = runEffect(*velocity, {{0, true, 60, 0.0f}, {1, true, 62, 1.0f}}, 512);
        auto driven = audio::makeMidiEffect(with(audio::MidiEffectType::Velocity, {{0, 100}}));
        const std::vector<audio::MidiEvent> lifted = runEffect(*driven, {{0, true, 60, 0.25f}}, 512);
        c.check(out.size() == 2 && std::fabs(out[0].velocity - 64.0f / 127.0f) < 1e-5f && std::fabs(out[1].velocity - 1.0f) < 1e-5f &&
                    lifted.size() == 1 && std::fabs(lifted[0].velocity - (1.0f + 126.0f * std::pow(0.25f, 0.25f)) / 127.0f) < 1e-4f,
                "MIDI effects: velocity's range and drive",
                std::to_string(out.size()) + (out.size() == 2 ? " " + std::to_string(out[0].velocity) + " " + std::to_string(out[1].velocity) : std::string()) +
                    (lifted.empty() ? std::string() : " " + std::to_string(lifted[0].velocity)));
    }

    // Bypassed: notes pass, and what it had made ends as it steps aside
    {
        audio::DeviceSetting s = audio::midiEffectSetting(audio::MidiEffectType::Chord);
        auto fx = audio::makeMidiEffect(s);
        std::vector<audio::MidiEvent> out = runEffect(*fx, {{0, true, 60, 0.8f}}, 512);
        s.bypass = true;
        fx->applySetting(s);
        out = runEffect(*fx, {{0, true, 62, 0.8f}}, 512);
        size_t offs = 0;
        for (const audio::MidiEvent &e : out)
            offs += e.on ? 0 : 1;
        c.check(offs == 3 && ons(out).size() == 1 && ons(out)[0].key == 62, "MIDI effects: bypass ends what was made and passes notes");
    }

    // Through a chain: an arpeggiator before the synth plays its steps on the grid, as heard
    {
        audio::DeviceChain chain;
        chain.prepare(kRate, kBlock);
        chain.apply({audio::midiEffectSetting(audio::MidiEffectType::Arpeggiator)}, audio::SynthDevice::setting(), {});
        audio::ChainInstrument instrument;
        instrument.setChain(&chain);
        const uint64_t total = 48000; // two beats: eight sixteenths
        std::vector<float> left(kBlock), right(kBlock), heard;
        audio::TransportState t;
        t.playing = true;
        t.sampleRate = kRate;
        t.tempo = 120.0;
        for (uint64_t start = 0; start < total; start += kBlock) {
            t.position = start;
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            instrument.automateBlock(t);
            instrument.beginBlock();
            if (start == 0) {
                const bool timed = instrument.timedEvents();
                instrument.noteOnAt(48, 0.9f, 0);
                instrument.noteOnAt(55, 0.9f, 0);
                if (!timed)
                    c.check(false, "MIDI effects: a chain with MIDI effects takes timed notes");
            }
            float *channels[2] = {left.data(), right.data()};
            audio::AudioBlock block{channels, 2, kBlock, kRate, t};
            instrument.process(block);
            heard.insert(heard.end(), left.begin(), left.end());
            chain.collect();
        }
        const audio::AudioAnalysis analysis = audio::analyseSamples(heard.data(), heard.size(), 1, kRate);
        size_t onGrid = 0;
        for (const audio::Transient &tr : analysis.transients) {
            const double step = static_cast<double>(tr.frame) / 6000.0;
            if (std::fabs(step - std::round(step)) * 6000.0 < 0.015 * kRate) // the analysis places onsets to about 10 ms
                ++onGrid;
        }
        std::string at;
        for (const audio::Transient &tr : analysis.transients)
            at += " " + std::to_string(tr.frame);
        c.check(analysis.transients.size() == 8 && onGrid == 8,
                "MIDI effects: an arpeggiator before the synth plays on the grid",
                std::to_string(analysis.transients.size()) + " notes heard, " + std::to_string(onGrid) + " on the grid:" + at);
    }

    return c.failures;
}

int runMidiEffectProjectChecks()
{
    Checker c;

    daw::Project p;
    p.tracks.resize(1);
    daw::Track &t = p.tracks[0];
    t.name = "Keys";
    t.kind = daw::TrackKind::Instrument;
    t.instrument = audio::SynthDevice::setting();
    t.midiEffects.push_back(with(audio::MidiEffectType::Arpeggiator, {{0, 2}, {2, 3}}));
    t.midiEffects.push_back(audio::midiEffectSetting(audio::MidiEffectType::Scale));
    t.midiEffects[1].bypass = true;

    // The file keeps them, in order; one where an effect goes is refused, and a MIDI effect as an effect too
    {
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool read = daw::readProject(text, back, error);
        c.check(read && back.tracks[0].midiEffects == p.tracks[0].midiEffects, "File: MIDI effects round trip", error);
        std::string moved = text;
        const size_t at = moved.find("[[tracks.midi_effects]]");
        moved.replace(at, 23, "[[tracks.effects]]");
        daw::Project refused;
        std::string why;
        c.check(!daw::readProject(moved, refused, why) && why.find("before the instrument") != std::string::npos,
                "File: a MIDI effect among the effects is refused", why);
        std::string unknown = text;
        unknown.replace(unknown.find("\"arpeggiator\""), 13, "\"harmonizer\"");
        c.check(!daw::readProject(unknown, refused, why) && why.find("unknown MIDI effect") != std::string::npos,
                "File: an unknown MIDI effect is refused", why);
    }

    // The history names their edits
    {
        daw::Project none = p;
        none.tracks[0].midiEffects.pop_back();
        daw::Project faster = p;
        faster.tracks[0].midiEffects[0].setValue(1, 5);
        daw::Project swapped = p;
        std::swap(swapped.tracks[0].midiEffects[0], swapped.tracks[0].midiEffects[1]);
        c.check(daw::describeChange(none, p) == "Add MIDI effect" && daw::describeChange(p, none) == "Remove MIDI effect" &&
                    daw::describeChange(p, faster) == "Arpeggiator settings" && daw::describeChange(p, swapped) == "Reorder MIDI effects" &&
                    !daw::sameContent(p, faster) && !daw::sameSaved(p, faster),
                "History: MIDI effect edits are named", daw::describeChange(p, faster));
    }

    return c.failures;
}

}
