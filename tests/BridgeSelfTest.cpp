#include "BridgeSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/AudioSystem.h"
#include "audio/DeviceChain.h"
#include "audio/Effects.h"
#include "audio/Midi.h"
#include "audio/Synth.h"
#include "audio/Track.h"
#include "daw/AudioBridge.h"
#include "daw/AudioPool.h"
#include "daw/AutomationLanes.h"
#include "daw/Format.h"
#include "daw/History.h"
#include "daw/Library.h"
#include "daw/Project.h"
#include "daw/ProjectStore.h"
#include "daw/Settings.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

namespace tests {

namespace {

using Processors = std::vector<audio::TrackProcessor *>;

// The processors of the first `count` tracks, in the bridge's (project) order
Processors processorsOf(const daw::AudioBridge &bridge, size_t count)
{
    Processors out;
    for (size_t i = 0; i < count; ++i)
        out.push_back(bridge.processor(static_cast<int>(i)));
    return out;
}

// Every entry non-null and no two equal
bool distinctNonNull(const Processors &p)
{
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == nullptr)
            return false;
        for (size_t j = 0; j < i; ++j) {
            if (p[i] == p[j])
                return false;
        }
    }
    return true;
}

// A project of `count` tracks, alternating instrument tracks (with no instrument, so nothing loads)
// and audio tracks, each with one empty clip
daw::Project makeProject(size_t count)
{
    daw::Project p;
    for (size_t i = 0; i < count; ++i) {
        daw::Track t;
        t.name = "Track " + std::to_string(i + 1);
        t.kind = i % 2 == 0 ? daw::TrackKind::Instrument : daw::TrackKind::Audio;
        t.clips.push_back(daw::Clip("Clip", 0.0, 4.0));
        p.tracks.push_back(t);
    }
    return p;
}

// The RMS of interleaved stereo frames [from, to)
double rmsOf(const std::vector<float> &stereo, size_t from, size_t to)
{
    double sum = 0.0;
    size_t n = 0;
    for (size_t f = from; f < to && f * 2 + 1 < stereo.size(); ++f, n += 2)
        sum += static_cast<double>(stereo[f * 2]) * stereo[f * 2] + static_cast<double>(stereo[f * 2 + 1]) * stereo[f * 2 + 1];
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

// Plays the engine from the start for `beats` beats at 120 BPM (24000 frames a beat at 48 kHz)
std::vector<float> playFromStart(audio::AudioSystem &engine, double beats)
{
    constexpr uint32_t kBlock = 512;
    engine.transport().setTempo(120.0);
    engine.transport().locate(0);
    engine.transport().play();
    std::vector<float> out(static_cast<size_t>(beats * 24000.0) * 2);
    for (size_t at = 0; at + kBlock * 2 <= out.size(); at += kBlock * 2)
        engine.renderOffline(out.data() + at, kBlock, 48000.0);
    engine.transport().stop();
    return out;
}

// Automation through the bridge: a device's parameter follows its lane at the playhead (playing or
// not), the synth's likewise, the track's volume follows its lane in the sound, and a lane with no
// points plays nothing
void automationChecks(Checker &c, audio::AudioSystem &engine, daw::AudioBridge &bridge,
                      const daw::AudioBridge::SyncOptions &options)
{
    bridge.dropAll();
    daw::Project p;
    daw::Track t;
    t.name = "Automated";
    t.kind = daw::TrackKind::Instrument;
    t.instrument = audio::SynthDevice::setting();
    t.effects.push_back(audio::effectSetting(audio::EffectType::Gain));
    // The Gain effect's gain (-60 to 24 dB) from a quarter of the way at beat 0 to three quarters at beat 4
    t.effects[0].automation.push_back({0, {{0.0, 0.25f}, {4.0, 0.75f}}});
    // A synth parameter (the first shown one that isn't a choice) at its knob's middle from beat 0
    const std::vector<audio::DeviceParam> synthParams = audio::synth::synthParams();
    const auto shown = std::find_if(synthParams.begin(), synthParams.end(), [](const audio::DeviceParam &q) {
        return !q.hidden && !q.readOnly && q.unit != audio::ParamUnit::Choice && q.unit != audio::ParamUnit::Toggle && q.max > q.min;
    });
    const audio::DeviceParam synthParam = shown != synthParams.end() ? *shown : audio::DeviceParam{};
    t.instrument.automation.push_back({synthParam.id, {{0.0, 0.5f}}});
    // A note held across the whole render
    t.clips.push_back(daw::Clip("Held", 0.0, 16.0, {daw::Note{0.0, 16.0, 57, 0.8f}}));
    daw::ProjectStore store(p);
    store.edit().tracks.push_back(t);
    bridge.sync(store, options);

    // Stopped at beat 2: the Gain effect sits halfway along its lane (-18 dB), the synth at its middle
    engine.transport().stop();
    engine.transport().locate(48000);
    std::vector<float> block(256 * 2);
    engine.renderOffline(block.data(), 256, 48000.0);
    engine.renderOffline(block.data(), 256, 48000.0);
    audio::DeviceChain *chain = bridge.chainOf(false, 0);
    auto *gain = chain ? dynamic_cast<audio::BuiltinEffect *>(chain->effect(0)) : nullptr;
    const float gainNow = gain ? gain->param(0) : 0.0f;
    c.check(gain && std::fabs(gainNow - -18.0f) < 0.01f, "Automation: an effect's parameter follows its lane at the playhead",
            std::to_string(gainNow) + " dB at beat 2");
    auto *synth = chain ? dynamic_cast<audio::SynthDevice *>(chain->instrument()) : nullptr;
    const double wanted = daw::automationCurve(synthParam).value(0.5);
    const double synthNow = synth ? synth->synth().param(synthParam.id) : -1.0;
    c.check(synth && std::fabs(synthNow - wanted) < 1e-3 * std::max(1.0, std::fabs(wanted)),
            "Automation: the synth's parameter follows its lane", synthParam.name + " " + std::to_string(synthNow));

    // The track's own volume: silent until beat 4, then at unity. The note sounds only from there,
    // as loud as without automation.
    store.editTrack(0).automation.push_back(
        {audio::kVolumeParam, {{0.0, 0.0f}, {3.999, 0.0f}, {4.0, daw::faderPosition(1.0f, 2.0f)}}});
    store.editTrack(0).effects[0].automation.clear(); // the Gain effect at its own 0 dB
    bridge.sync(store, options);
    const std::vector<float> automatedRender = playFromStart(engine, 5.5);
    const double before = rmsOf(automatedRender, 24000, 90000), after = rmsOf(automatedRender, 102000, 130000);
    store.editTrack(0).automation.clear();
    bridge.sync(store, options);
    const std::vector<float> plainRender = playFromStart(engine, 5.5);
    const double plain = rmsOf(plainRender, 102000, 130000);
    const double offDb = 20.0 * std::log10(std::max(after, 1e-12) / std::max(plain, 1e-12));
    c.check(before < 1e-5 && after > 1e-3 && std::fabs(offDb) < 0.5,
            "Automation: the track's volume follows its lane (silent, then as loud as without)",
            "rms " + std::to_string(before) + " then " + std::to_string(after) + " (" + std::to_string(offDb) + " dB off)");

    // A lane with no points is kept but plays nothing: the effect is back at its own value
    store.editTrack(0).effects[0].automation = {{0, {}}};
    store.editDevice(daw::DevicePlace::effect(false, 0, 0))->setValue(0, -6.0);
    bridge.sync(store, options);
    engine.renderOffline(block.data(), 256, 48000.0);
    const float own = gain ? gain->param(0) : 0.0f;
    c.check(gain && std::fabs(own - -6.0f) < 0.01f, "Automation: a lane with no points leaves the parameter its own value",
            std::to_string(own) + " dB");

    // A plugin's report of an automated parameter isn't written into the project (checked through the
    // project's compare: the lane counts, not a value)
    c.check(audio::automated(store.project().tracks[0].instrument.automation, synthParam.id) &&
                !audio::automated(store.project().tracks[0].effects[0].automation, 0),
            "Automation: a lane with points is automated, one without isn't");
    bridge.dropAll();
}

}

int runBridgeChecks()
{
    std::printf("Bridge self-test: processors kept across structural edits\n\n");
    Checker c;

    // A deviceless engine with its MIDI hub first (as RenderJob makes it), an empty library and a pool
    const std::string dir = (std::filesystem::temp_directory_path() / "vibin-bridge-selftest").u8string();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);

    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::Settings settings;
    settings.libraryPath = dir;
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    audio::MidiHub &midi = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, midi, nullptr, library, pool);

    daw::AudioBridge::SyncOptions options;
    options.listen = false; // as an export runs: no MIDI listening

    daw::ProjectStore store(makeProject(4));

    // Four tracks: four processors, all distinct
    bridge.sync(store, options);
    c.check(bridge.processorsMade() == 4, "Bridge: four tracks make four processors",
            std::to_string(bridge.processorsMade()) + " made");
    const Processors first = processorsOf(bridge, 4);
    c.check(distinctNonNull(first) && bridge.processor(4) == nullptr, "Bridge: each track has its own processor",
            "a track past the last has none");

    // Delete track 1: the others keep their processors, moved up; one is removed, none made
    const daw::Project beforeDelete = store.project();
    const uint64_t madeBeforeDelete = bridge.processorsMade();
    const uint64_t removedBeforeDelete = bridge.processorsRemoved();
    store.edit().deleteTrack(1);
    bridge.sync(store, options);
    const Processors afterDelete = processorsOf(bridge, 3);
    c.check(afterDelete == Processors{first[0], first[2], first[3]} && bridge.processor(3) == nullptr,
            "Bridge: a delete keeps the others' processors, moved up");
    c.check(bridge.processorsMade() == madeBeforeDelete, "Bridge: a delete makes no processor");
    c.check(bridge.processorsRemoved() == removedBeforeDelete + 1, "Bridge: a delete removes exactly one processor",
            std::to_string(bridge.processorsRemoved() - removedBeforeDelete) + " removed");

    // Undo, through the history as DawApp does it (a structural plan, so the store replaces the project): the
    // project as it was before the delete (its ids kept). Tracks 0, 2, 3 keep their processors at their
    // indexes; only the restored track 1 is new
    daw::History history;
    history.reset(beforeDelete);
    history.update(store, true, false, 1.0);
    const uint64_t madeBeforeUndo = bridge.processorsMade();
    const uint64_t removedBeforeUndo = bridge.processorsRemoved();
    daw::HistoryRestore undoDelete;
    const bool deleteUndone = history.undo(store, 2.0, undoDelete);
    store.restore(std::move(undoDelete));
    history.adopt(store);
    c.check(deleteUndone && store.project().tracks.size() == beforeDelete.tracks.size(), "Bridge: the history undoes the delete");
    bridge.sync(store, options);
    const Processors afterUndo = processorsOf(bridge, 4);
    c.check(afterUndo[0] == first[0] && afterUndo[2] == first[2] && afterUndo[3] == first[3],
            "Bridge: undo keeps the processors of the kept tracks");
    c.check(afterUndo[1] != nullptr && afterUndo[1] != first[0] && afterUndo[1] != first[2] && afterUndo[1] != first[3],
            "Bridge: undo gives the restored track a new processor");
    c.check(bridge.processorsMade() == madeBeforeUndo + 1 && bridge.processorsRemoved() == removedBeforeUndo,
            "Bridge: undo makes exactly one processor", std::to_string(bridge.processorsMade() - madeBeforeUndo) + " made");

    // Reorder: the last track moves to the front; the processors follow their tracks
    const Processors beforeReorder = processorsOf(bridge, 4);
    const uint64_t madeBeforeReorder = bridge.processorsMade();
    const uint64_t removedBeforeReorder = bridge.processorsRemoved();
    {
        daw::Project &p = store.edit();
        std::rotate(p.tracks.begin(), p.tracks.end() - 1, p.tracks.end());
    }
    bridge.sync(store, options);
    const Processors afterReorder = processorsOf(bridge, 4);
    c.check(afterReorder == Processors{beforeReorder[3], beforeReorder[0], beforeReorder[1], beforeReorder[2]},
            "Bridge: a reorder moves the processors with their tracks");
    c.check(bridge.processorsMade() == madeBeforeReorder && bridge.processorsRemoved() == removedBeforeReorder,
            "Bridge: a reorder makes and removes nothing");

    // Kind change: the track keeps its id but gets a new processor; the others are kept
    const daw::ObjectId changedId = store.project().tracks[0].id;
    const Processors beforeKind = processorsOf(bridge, 4);
    const uint64_t madeBeforeKind = bridge.processorsMade();
    const uint64_t removedBeforeKind = bridge.processorsRemoved();
    {
        daw::Track &t = store.editTrack(0);
        t.kind = t.kind == daw::TrackKind::Instrument ? daw::TrackKind::Audio : daw::TrackKind::Instrument;
    }
    bridge.sync(store, options);
    const Processors afterKind = processorsOf(bridge, 4);
    c.check(store.project().tracks[0].id == changedId && afterKind[0] != nullptr &&
                afterKind[0] != afterKind[1] && afterKind[0] != afterKind[2] && afterKind[0] != afterKind[3],
            "Bridge: a kind change gives that track a new processor");
    c.check(afterKind[1] == beforeKind[1] && afterKind[2] == beforeKind[2] && afterKind[3] == beforeKind[3],
            "Bridge: a kind change keeps the other processors");
    c.check(bridge.processorsMade() == madeBeforeKind + 1 && bridge.processorsRemoved() == removedBeforeKind + 1,
            "Bridge: a kind change makes one and removes one");

    // An edit to the mix (gain) changes no track: nothing is made or removed, and the processors stay
    const Processors beforeMix = processorsOf(bridge, 4);
    const uint64_t madeBeforeMix = bridge.processorsMade();
    const uint64_t removedBeforeMix = bridge.processorsRemoved();
    store.editMix(2).gain = 0.5f;
    bridge.sync(store, options);
    c.check(processorsOf(bridge, 4) == beforeMix && bridge.processorsMade() == madeBeforeMix &&
                bridge.processorsRemoved() == removedBeforeMix,
            "Bridge: a mix edit keeps every processor and makes nothing");

    // dropAll: every track is made again (the processors that were there are gone)
    const uint64_t madeBeforeDrop = bridge.processorsMade();
    bridge.dropAll();
    bridge.sync(store, options);
    const Processors afterDrop = processorsOf(bridge, 4);
    c.check(bridge.processorsMade() == madeBeforeDrop + 4 && distinctNonNull(afterDrop),
            "Bridge: dropAll then sync makes every track new",
            std::to_string(bridge.processorsMade() - madeBeforeDrop) + " made");

    // A track added at the end: one processor made, the others kept
    const Processors beforeAdd = processorsOf(bridge, 4);
    const uint64_t madeBeforeAdd = bridge.processorsMade();
    const uint64_t removedBeforeAdd = bridge.processorsRemoved();
    store.edit().addTrack(daw::TrackKind::Audio);
    bridge.sync(store, options);
    const Processors afterAdd = processorsOf(bridge, 5);
    c.check(afterAdd[0] == beforeAdd[0] && afterAdd[1] == beforeAdd[1] && afterAdd[2] == beforeAdd[2] &&
                afterAdd[3] == beforeAdd[3] && afterAdd[4] != nullptr,
            "Bridge: an added track gets a processor, the others keep theirs");
    c.check(bridge.processorsMade() == madeBeforeAdd + 1 && bridge.processorsRemoved() == removedBeforeAdd,
            "Bridge: an added track makes exactly one processor");

    // Undoing a one-note edit on one track: the bridge sends that track's notes again and no other's, and
    // makes and removes no processor
    {
        const int instrument = static_cast<int>(
            std::find_if(store.project().tracks.begin(), store.project().tracks.end(),
                         [](const daw::Track &t) { return t.kind == daw::TrackKind::Instrument; }) -
            store.project().tracks.begin());
        daw::Note note;
        note.start = 0.5;
        store.editClip(instrument, 0).notes.push_back(note);
        bridge.sync(store, options);
        daw::History noteHistory;
        noteHistory.reset(store.project());
        store.editClip(instrument, 0).notes[0].start = 2.0;
        noteHistory.update(store, true, false, 1.0);
        bridge.sync(store, options);
        const uint64_t sent = bridge.sequencesSent(), made = bridge.processorsMade(), removed = bridge.processorsRemoved();
        daw::HistoryRestore plan;
        const bool undone = noteHistory.undo(store, 2.0, plan);
        store.restore(std::move(plan));
        noteHistory.adopt(store);
        bridge.sync(store, options);
        c.check(undone && store.project().tracks[static_cast<size_t>(instrument)].clips[0].notes[0].start == 0.5,
                "Bridge: undoing a note edit puts the note back");
        c.check(bridge.sequencesSent() == sent + 1, "Bridge: undoing a one-note edit sends only that track's notes",
                std::to_string(bridge.sequencesSent() - sent) + " sent");
        c.check(bridge.processorsMade() == made && bridge.processorsRemoved() == removed,
                "Bridge: undoing a one-note edit makes and removes no processor");
    }

    automationChecks(c, engine, bridge, options);

    return c.finish("bridge");
}

}
