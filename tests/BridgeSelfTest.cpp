#include "BridgeSelfTest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/AudioSystem.h"
#include "audio/Midi.h"
#include "audio/Track.h"
#include "daw/AudioBridge.h"
#include "daw/AudioPool.h"
#include "daw/Library.h"
#include "daw/Project.h"
#include "daw/ProjectStore.h"
#include "daw/Settings.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

namespace tests {

namespace {

struct Checker {
    int failures = 0;

    void check(bool ok, const std::string &what, const std::string &detail = {})
    {
        std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
        if (!ok)
            ++failures;
    }
};

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

    // Undo: the project as it was before the delete (its ids kept). Tracks 0, 2, 3 keep their processors
    // at their indexes; only the restored track 1 is new
    const uint64_t madeBeforeUndo = bridge.processorsMade();
    const uint64_t removedBeforeUndo = bridge.processorsRemoved();
    store.replace(beforeDelete);
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

    std::printf("\n%s: %d failed\n", c.failures == 0 ? "All passed" : "FAILED", c.failures);
    return c.failures;
}

}
