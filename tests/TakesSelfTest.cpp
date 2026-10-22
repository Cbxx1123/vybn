#include "TakesSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/AudioSystem.h"
#include "audio/Midi.h"
#include "audio/Recorder.h"
#include "daw/audio/AudioBridge.h"
#include "daw/audio/AudioPool.h"
#include "daw/audio/Recording.h"
#include "daw/app/Settings.h"
#include "daw/io/ProjectFile.h"
#include "daw/library/Library.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"
#include "daw/model/Takes.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

namespace tests {

namespace {

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::fabs(a - b) <= tolerance;
}

// An audio take: `file` from `offset` seconds, at `start` for `length` beats
daw::Clip audioTake(daw::ObjectId id, const std::string &file, double start, double length, double offset = 0.0)
{
    daw::Clip c("Take", start, length);
    c.id = id;
    c.audioFile = file;
    c.offset = offset;
    return c;
}

// The comp as "take@start-end" pieces, for details
std::string describe(const daw::Track &t)
{
    std::string text;
    for (const daw::Clip &c : t.clips)
        text += std::to_string(c.take) + "@" + std::to_string(c.start).substr(0, 4) + "-" + std::to_string(c.end()).substr(0, 4) + " ";
    return text;
}

// An audio track with two lanes, one take each over beats 0-8 (files a and b), at 120 BPM
daw::Track twoTakes()
{
    daw::Track t;
    t.id = 1;
    t.kind = daw::TrackKind::Audio;
    t.takeLanes.push_back({10, "Take 2", {audioTake(12, "b.wav", 0.0, 8.0)}});
    t.takeLanes.push_back({11, "Take 1", {audioTake(13, "a.wav", 0.0, 8.0)}});
    return t;
}

// Loop recording through RecordingSession on a deviceless engine: an armed audio track (its input a
// ramp, so the file says which frame was where) and an armed instrument track (a note played in each
// lap), two and a half laps of a one-bar loop at 120 BPM (2 s a lap). Each lap becomes a take; the
// comp is the newest take wherever it reaches.
void recordingChecks(Checker &c)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "vybn-takes-selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    audio::MidiHub &hub = engine.add<audio::MidiHub>();
    audio::InputRecorder &recorder = engine.add<audio::InputRecorder>();
    engine.setOfflineInputs(1);

    daw::Project p;
    daw::Track vox;
    vox.name = "Vox";
    vox.kind = daw::TrackKind::Audio;
    vox.armed = true;
    daw::Track keys;
    keys.name = "Keys";
    keys.kind = daw::TrackKind::Instrument;
    keys.armed = true;
    p.tracks = {vox, keys};
    p.loopStart = 0.0;
    p.loopEnd = 4.0;
    p.looping = true;
    daw::ProjectStore store(p);
    audio::Transport &transport = engine.transport();
    transport.setTempo(120.0);
    transport.setLoop(0.0, 4.0);
    transport.setLooping(true);
    transport.locate(0);
    engine.renderOffline(nullptr, 0, 48000.0); // applies the locate

    daw::RecordingSession session(store, engine, hub, recorder, pool);
    std::string why;
    const bool started = session.start(dir.u8string(), why);

    // 2.5 laps in blocks of 480 frames, the input counting frames; a note in each lap at its second beat
    constexpr uint32_t kBlock = 480;
    std::vector<float> out(kBlock * 2), input(kBlock);
    uint64_t frame = 0;
    const int keysPlayed[] = {60, 62, 64};
    for (int block = 0; block < 500; ++block) { // 200 blocks a lap
        const uint32_t lap = transport.laps();
        if (lap < 3 && block % 200 == 25)
            hub.send(audio::MidiMessage::noteOn(keysPlayed[lap], 100));
        if (lap < 3 && block % 200 == 35)
            hub.send(audio::MidiMessage::noteOff(keysPlayed[lap]));
        for (uint32_t i = 0; i < kBlock; ++i)
            input[i] = static_cast<float>(frame++) / 1e6f;
        engine.renderOffline(out.data(), kBlock, 48000.0, input.data(), 1);
        session.update();
    }
    transport.stop();
    for (int i = 0; i < 4 && (session.recording() || session.finishing()); ++i) {
        engine.renderOffline(out.data(), kBlock, 48000.0, input.data(), 1);
        session.update();
    }
    const std::vector<daw::ObjectId> opened = session.takeNewTakes();

    const daw::Track &a = store.project().tracks[0];
    const daw::Track &k = store.project().tracks[1];
    bool audioTakes = started && a.takeLanes.size() == 3 && a.takeLanes[0].name == "Take 3" && a.takeLanes[2].name == "Take 1";
    if (audioTakes) {
        const daw::Clip &first = a.takeLanes[2].clips[0], &second = a.takeLanes[1].clips[0], &third = a.takeLanes[0].clips[0];
        audioTakes = near(first.start, 0.0) && near(first.length, 4.0, 1e-3) && near(second.offset, 2.0, 1e-4) &&
                     near(second.length, 4.0, 1e-3) && near(third.offset, 4.0, 1e-4) && third.length < 4.0 &&
                     !first.audioFile.empty() && first.audioFile == third.audioFile;
    }
    // The comp: the short last lap, then the lap before it
    const bool audioComp = a.clips.size() == 2 && a.clips[0].take == a.takeLanes[0].clips[0].id &&
                           a.clips[1].take == a.takeLanes[1].clips[0].id && near(a.clips[1].offset, 2.0 + a.clips[1].start * 0.5, 1e-4);
    c.check(audioTakes && audioComp, "Takes: loop recording audio makes a take of each lap, in one file",
            why + " " + std::to_string(a.takeLanes.size()) + " lanes, " + describe(a));
    // The file holds the ramp: the second take's first frame is frame 96000 of the input
    std::string error;
    auto file = a.takeLanes.size() == 3 ? audio::loadAudioFile(a.takeLanes[1].clips[0].audioFile, error) : nullptr;
    c.check(file && near(file->samples[96000], 96000.0 / 1e6, 1e-6), "Takes: a lap's take starts on its sample", error);

    bool midiTakes = k.takeLanes.size() == 3;
    for (size_t l = 0; midiTakes && l < 3; ++l) {
        const daw::Clip &take = k.takeLanes[l].clips[0];
        midiTakes = take.notes.size() == 1 && take.notes[0].key == keysPlayed[2 - l] && near(take.start, 0.0);
    }
    c.check(midiTakes && k.clips.size() == 1 && k.clips[0].notes.size() == 1 && k.clips[0].notes[0].key == 64 && opened.size() == 2,
            "Takes: loop recording MIDI makes a take of each lap", std::to_string(k.takeLanes.size()) + " lanes");
    fs::remove_all(dir, ec);
}

// The comp played through the bridge: two takes of steady levels (0.5 and -0.5), the comp switching
// from one to the other at beat 2. The switch is a 10 ms crossfade, not a step; auditioning the
// second take's lane plays it alone.
void playbackChecks(Checker &c)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "vybn-takes-play-selftest";
    fs::create_directories(dir, ec);
    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::Settings settings;
    settings.libraryPath = dir.u8string();
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    audio::MidiHub &hub = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, hub, nullptr, library, pool);
    daw::AudioBridge::SyncOptions options;
    options.listen = false;

    const size_t frames = 48000 * 4; // 4 s: 8 beats at 120
    pool.addRecording("high.wav", 48000.0, 1, std::vector<float>(frames, 0.5f));
    pool.addRecording("low.wav", 48000.0, 1, std::vector<float>(frames, -0.5f));
    daw::Project p;
    daw::Track t;
    t.kind = daw::TrackKind::Audio;
    t.takeLanes.push_back({10, "Take 2", {audioTake(12, "low.wav", 0.0, 8.0)}});
    t.takeLanes.push_back({11, "Take 1", {audioTake(13, "high.wav", 0.0, 8.0)}});
    daw::useLane(t, 11, p.timing);
    daw::compRange(t, 2.0, 8.0, 12, p.timing);
    p.tracks.push_back(t);
    daw::ProjectStore store(p);
    bridge.sync(store, options);

    auto play = [&] {
        engine.transport().setTempo(120.0);
        engine.transport().locate(0);
        engine.transport().play();
        std::vector<float> out(48000 * 2 * 2); // 2 s
        for (size_t at = 0; at + 512 * 2 <= out.size(); at += 512 * 2) {
            bridge.sync(store, options);
            engine.renderOffline(out.data() + at, 512, 48000.0);
        }
        engine.transport().stop();
        return out;
    };
    const std::vector<float> comp = play();
    // Around beat 2 (frame 48000) the level goes from high to low over about 480 frames, no step
    float jump = 0.0f;
    for (size_t f = 47000; f < 49000; ++f)
        jump = std::max(jump, std::fabs(comp[(f + 1) * 2] - comp[f * 2]));
    const float before = comp[40000 * 2], after = comp[56000 * 2];
    c.check(before > 0.2f && after < -0.2f && jump < 0.01f, "Takes: a comp's sections crossfade where they meet",
            "largest step " + std::to_string(jump));

    bridge.setAudition(store.project().tracks[0].id, 10);
    const std::vector<float> heard = play();
    c.check(heard[20000 * 2] < -0.2f && heard[60000 * 2] < -0.2f, "Takes: auditioning a lane plays that take instead of the comp",
            std::to_string(heard[20000 * 2]));
    fs::remove_all(dir, ec);
}

}

int runTakesChecks()
{
    Checker c;
    const audio::TempoMap timing(120.0);

    // Slicing: the offset follows the start, through a tempo change too; notes and curves keep their place
    {
        audio::TempoMap change;
        change.setPoints({{0.0, 120.0, false}, {4.0, 60.0, false}});
        daw::Clip clip = audioTake(1, "x.wav", 2.0, 6.0, 0.5); // 2 to 8
        const daw::Clip cut = daw::sliceClip(clip, 5.0, 7.0, change);
        daw::Clip midi("Riff", 0.0, 8.0, {{1.0, 0.5, 60, 0.8f}, {3.0, 2.0, 62, 0.8f}, {6.0, 1.0, 64, 0.8f}});
        midi.controls.push_back({1, {{0.0, 0.0f}, {8.0, 1.0f}}});
        const daw::Clip part = daw::sliceClip(midi, 2.0, 6.0, timing);
        // 2 to 4 is 1 s at 120, 4 to 5 another at 60: 0.5 + 2 s
        c.check(near(cut.start, 5.0) && near(cut.length, 2.0) && near(cut.offset, 2.5) && part.notes.size() == 1 &&
                    near(part.notes[0].start, 1.0) && part.controls.size() == 1 && near(part.controls[0].points.front().value, 0.25, 1e-6) &&
                    near(part.controls[0].points.back().value, 0.75, 1e-6),
                "Takes: a slice keeps its sound, notes and curves in place", "offset " + std::to_string(cut.offset));
    }

    // Comping a range from another take, then more of it: the sections join up
    {
        daw::Track t = twoTakes();
        daw::useLane(t, 11, timing); // all of take 1 (a.wav)
        const bool whole = t.clips.size() == 1 && t.clips[0].take == 13;
        daw::compRange(t, 2.0, 4.0, 12, timing);
        const bool three = t.clips.size() == 3 && t.clips[0].take == 13 && t.clips[1].take == 12 && t.clips[2].take == 13 &&
                           near(t.clips[1].offset, 1.0) && near(t.clips[2].offset, 2.0) && near(t.clips[0].end(), 2.0);
        daw::compRange(t, 4.0, 8.0, 12, timing);
        const bool joined = t.clips.size() == 2 && t.clips[1].take == 12 && near(t.clips[1].start, 2.0) && near(t.clips[1].end(), 8.0);
        const bool again = !daw::compRange(t, 3.0, 5.0, 12, timing); // already so: no edit
        c.check(whole && three && joined && again, "Takes: comping a range cuts the comp and joins sections of one take", describe(t));

        // A click's range: the section under it, or the gap between neighbours
        const auto around = daw::sectionAround(t, 5.0);
        daw::Track gap = twoTakes();
        gap.clips.push_back(audioTake(0, "c.wav", 0.0, 2.0));
        gap.clips.push_back(audioTake(0, "c.wav", 6.0, 2.0));
        const auto between = daw::sectionAround(gap, 3.0);
        c.check(near(around.first, 2.0) && near(around.second, 8.0) && near(between.first, 2.0) && near(between.second, 6.0),
                "Takes: a click's range is its section, or the gap it is in");

        // The edge between the sections moves: right grows the left one, left the right one
        const std::vector<double> edges = daw::compBoundaries(t);
        daw::moveBoundary(t, 2.0, 3.0, timing);
        const bool right = t.clips.size() == 2 && near(t.clips[0].end(), 3.0) && near(t.clips[1].start, 3.0) && near(t.clips[1].offset, 1.5);
        daw::moveBoundary(t, 3.0, 1.0, timing);
        const bool left = t.clips.size() == 2 && near(t.clips[0].end(), 1.0) && near(t.clips[1].start, 1.0) && near(t.clips[1].offset, 0.5);
        c.check(edges.size() == 1 && near(edges[0], 2.0) && right && left, "Takes: a boundary moves, both sections following", describe(t));

        // Stepping a section to the take below and back
        const bool down = daw::stepTake(t, 1, 1, timing) && t.clips.size() == 1 && t.clips[0].take == 13;
        const bool up = daw::stepTake(t, 0, -1, timing) && t.clips[0].take == 12;
        c.check(down && up && !daw::stepTake(t, 0, -1, timing), "Takes: a section steps to the take above or below");

        // What a lane plays, for drawing
        daw::compRange(t, 4.0, 6.0, 13, timing);
        const auto used = daw::laneUse(t, 11);
        c.check(used.size() == 1 && near(used[0].first, 4.0) && near(used[0].second, 6.0), "Takes: a lane's used ranges");
    }

    // MIDI takes: the comp holds each take's notes in its ranges
    {
        daw::Track t;
        t.kind = daw::TrackKind::Instrument;
        daw::Clip one("Take", 0.0, 4.0, {{0.0, 1.0, 60, 0.8f}, {2.0, 1.0, 62, 0.8f}});
        one.id = 21;
        daw::Clip two("Take", 0.0, 4.0, {{0.5, 1.0, 72, 0.8f}, {2.5, 1.0, 74, 0.8f}});
        two.id = 22;
        t.takeLanes.push_back({30, "Take 2", {two}});
        t.takeLanes.push_back({31, "Take 1", {one}});
        daw::useLane(t, 31, timing);
        daw::compRange(t, 2.0, 4.0, 22, timing);
        const bool ok = t.clips.size() == 2 && t.clips[0].notes.size() == 1 && t.clips[0].notes[0].key == 60 &&
                        t.clips[1].notes.size() == 1 && t.clips[1].notes[0].key == 74 && near(t.clips[1].notes[0].start, 0.5);
        c.check(ok, "Takes: MIDI sections hold their take's notes", describe(t));
    }

    // Folding a recording: one pass over empty space stays a clip; three laps become takes, the newest
    // on top and in the comp; recording over a clip keeps it in an "Original" lane
    {
        daw::Track plain;
        plain.kind = daw::TrackKind::Audio;
        const bool single = !daw::foldTakes(plain, {audioTake(5, "r.wav", 4.0, 4.0)}, timing) && plain.clips.size() == 1 &&
                            plain.takeLanes.empty();

        daw::Track laps;
        laps.kind = daw::TrackKind::Audio;
        laps.clips.push_back(audioTake(40, "old.wav", 0.0, 16.0)); // recorded over
        std::vector<daw::Clip> passes = {audioTake(41, "r.wav", 4.0, 4.0, 0.0), audioTake(42, "r.wav", 4.0, 4.0, 2.0),
                                         audioTake(43, "r.wav", 4.0, 2.0, 4.0)}; // the last lap cut short
        const bool folded = daw::foldTakes(laps, passes, timing);
        const bool lanes = laps.takeLanes.size() == 4 && laps.takeLanes[0].name == "Take 3" && laps.takeLanes[2].name == "Take 1" &&
                           laps.takeLanes[3].name == "Original";
        // The comp: the original before and after, the short last lap, then the lap before it
        const bool comp = laps.clips.size() == 4 && laps.clips[0].take == 40 && near(laps.clips[0].end(), 4.0) &&
                          laps.clips[1].take == 43 && near(laps.clips[1].end(), 6.0) && laps.clips[2].take == 42 &&
                          near(laps.clips[2].offset, 3.0) && near(laps.clips[2].end(), 8.0) && laps.clips[3].take == 40 &&
                          near(laps.clips[3].start, 8.0) && near(laps.clips[3].offset, 4.0);
        c.check(single && folded && lanes && comp, "Takes: a recording folds into takes, the newest wherever it reaches",
                describe(laps));

        // A new recording numbers on
        const bool more = daw::foldTakes(laps, {audioTake(44, "s.wav", 0.0, 2.0)}, timing) && laps.takeLanes[0].name == "Take 4" &&
                          laps.clips[0].take == 44;
        c.check(more, "Takes: later takes number on, in a lane on top");

        // Removing a lane leaves what was cut from it as ordinary clips; deleting unused takes; flattening
        daw::Track trimmed = laps;
        daw::removeLane(trimmed, trimmed.takeLanes[0].id);
        const bool removed = trimmed.takeLanes.size() == 4 && trimmed.clips[0].take == daw::kNoId;
        daw::Track unused = laps;
        daw::deleteUnusedTakes(unused);
        const bool pruned = unused.takeLanes.size() == 4; // Take 1 wasn't used anywhere
        daw::Track flat = laps;
        daw::flattenComp(flat);
        bool ordinary = flat.takeLanes.empty();
        for (const daw::Clip &clip : flat.clips)
            ordinary = ordinary && clip.take == daw::kNoId;
        c.check(removed && pruned && ordinary, "Takes: removing a lane, deleting unused takes and flattening");
    }

    // The file keeps the lanes and the sections' takes; ids stay unique when a track is copied
    {
        daw::Project p;
        p.tracks.push_back(twoTakes());
        daw::Track &t = p.tracks.back();
        t.name = "Vox";
        daw::useLane(t, 11, timing);
        daw::compRange(t, 2.0, 4.0, 12, timing);
        daw::Track midi;
        midi.kind = daw::TrackKind::Instrument;
        midi.name = "Keys";
        daw::Clip take("Take", 0.0, 4.0, {{1.0, 1.0, 60, 0.8f}});
        take.id = 50;
        midi.takeLanes.push_back({51, "Take 1", {take}});
        daw::useLane(midi, 51, timing);
        p.tracks.push_back(midi);
        daw::ObjectId next = 1;
        p.normalizeIds(next);
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool read = daw::readProject(text, back, error);
        const bool same = read && back.tracks.size() == 2 && back.tracks[0].takeLanes.size() == 2 && back.tracks[0].clips.size() == 3 &&
                          back.tracks[0].clips[1].take == 12 && back.tracks[0].takeLanes[1].clips[0].audioFile == "a.wav" &&
                          back.tracks[1].takeLanes.size() == 1 && back.tracks[1].clips[0].take == 50 && back.tracks[1].clips[0].notes.size() == 1 &&
                          daw::writeProject(back) == text;
        c.check(same, "Takes: lanes, takes and comp sections survive the file", error);

        daw::Project copied = back;
        copied.tracks.push_back(copied.tracks[0]);
        copied.normalizeIds(next);
        const daw::Track &copy = copied.tracks.back();
        const daw::Clip *source = daw::findTake(copy, copy.clips[1].take);
        c.check(copy.takeLanes[0].clips[0].id != 12 && source && source->audioFile == "b.wav",
                "Takes: a copied track's takes get fresh ids, its sections follow them");

        // A file naming a take the track hasn't: an ordinary clip
        std::string dangling = text;
        const size_t at = dangling.find("take = 12");
        dangling.replace(at, 9, "take = 99");
        daw::Project odd;
        c.check(daw::readProject(dangling, odd, error) && odd.tracks[0].clips[1].take == daw::kNoId,
                "Takes: a section naming no take reads as an ordinary clip", error);
    }

    // Undo names the edits
    {
        daw::Project a;
        a.tracks.push_back(twoTakes());
        daw::useLane(a.tracks[0], 11, timing);
        daw::Project comped = a;
        daw::compRange(comped.tracks[0], 2.0, 4.0, 12, timing);
        daw::Project flattened = comped;
        daw::flattenComp(flattened.tracks[0]);
        daw::Project recorded = a;
        daw::foldTakes(recorded.tracks[0], {audioTake(60, "n.wav", 0.0, 4.0)}, timing);
        c.check(daw::describeChange(a, comped) == "Comp" && daw::describeChange(comped, flattened) == "Flatten comp" &&
                    daw::describeChange(a, recorded) == "Record takes" && !daw::sameContent(a, comped),
                "Takes: undo names comping, flattening and recording", daw::describeChange(a, comped));
    }

    recordingChecks(c);
    playbackChecks(c);
    return c.failures;
}

}
