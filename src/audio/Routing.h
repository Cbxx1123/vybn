#pragma once

#include <cstdint>
#include <vector>

namespace audio {

class AudioProcessor;
class TrackProcessor;

// Where tracks' sound goes, as the audio thread plays it (docs/design/routing-plan.md). Without one, the
// AudioSystem adds every track straight into the master.

// The most sends one track can have (each takes a level slot in its TrackProcessor, 12 bytes: the
// project refuses more, so none is dropped silently)
constexpr uint32_t kMaxSends = 64;

// A share of a track's sound into a bus (a return or group track's TrackProcessor)
struct RouteSend {
    TrackProcessor *to = nullptr;
    uint32_t slot = 0;     // which of the track's send levels it takes (TrackProcessor::setSendLevel)
    uint32_t param = 0;    // the lane among the track's mix curves that automates it (0 for none)
    uint32_t delay = 0;    // frames it is held back (delay compensation), at most TrackProcessor::kMaxCompensation
    bool preFader = false; // taken before the track's gain and pan (still silenced by its mute)
};

// What a bus takes in: another track's output (send null), or one of its sends
struct Feed {
    TrackProcessor *from = nullptr;
    const RouteSend *send = nullptr;
};

// The most sidechains one track's effects, or the master's, can have (the engine has a key buffer for
// each; the project refuses more)
constexpr uint32_t kMaxKeys = 4;

// A sidechain: the sound of another track, after its effects and before its fader (not silenced by
// its mute), read from its delay line `delay` frames back, that one of this track's effects listens
// to in place of its own input. It carries no sound to the master.
struct KeyFeed {
    TrackProcessor *from = nullptr;
    uint32_t effect = 0; // the index of the keyed effect in the track's chain (or the master's)
    uint32_t delay = 0;  // frames it is held back, at most TrackProcessor::kMaxCompensation
};

// One track: where its sound goes after its gain and pan (held back by its own `compensation`), and,
// for a bus, what feeds it (filled in by Routing::finish)
struct Route {
    TrackProcessor *track = nullptr;
    TrackProcessor *output = nullptr; // a bus (a group track's processor), or nullptr for the master
    std::vector<RouteSend> sends;
    std::vector<Feed> feeds;
    std::vector<KeyFeed> keys; // at most TrackProcessor::kMaxKeys, each from a track in an earlier stage
};

// The whole graph, in stages: every track of a stage renders at once (across the real-time pool), a
// bus taking in what feeds it as it renders, so a bus is in a later stage than everything that feeds
// it; then the stage's tracks that go to the master are added into it in order. Made on the main
// thread and never changed once handed to AudioSystem::setRouting.
struct Routing {
    std::vector<Route> routes;       // stage by stage
    std::vector<uint32_t> stageEnds; // where each stage ends in routes
    std::vector<KeyFeed> masterKeys;  // the master's effects keyed by tracks (read once every stage has rendered)

    // Begins a stage; add appends a route to it (beginning the first if none was)
    void beginStage();
    void add(Route route);
    // Once every route is in: each bus's feeds, from the outputs and sends into it, in route order
    void finish();
    // Whether a processor plays any part in it
    bool contains(const AudioProcessor *processor) const;
};

}
