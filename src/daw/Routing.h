#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "Project.h"
#include "audio/Routing.h"

namespace daw {

class ProjectStore;

// Routing between tracks (docs/routing-plan.md): each track's output (the master, or a group track)
// and its sends (into return or group tracks). These are the project-side rules and the plan the audio side
// is built from; audio/Routing.h is what the audio thread plays.

// A send's automation lane is one of its track's own lanes (Track::automation), with this parameter
// id for the return it sends to, beside audio::kVolumeParam and kPanParam
constexpr uint32_t kSendParamBase = 0x40000000u;
constexpr uint32_t sendParam(ObjectId to) { return kSendParamBase + to; }
constexpr bool isSendParam(uint32_t param) { return param > kSendParamBase && param < 0xFFFF0000u; }
constexpr ObjectId sendTarget(uint32_t param) { return isSendParam(param) ? param - kSendParamBase : kNoId; }

// The letters returns go by (the mixer's send knobs): 0 is "A", 25 "Z", 26 "AA"
std::string returnLetter(int index);
// A return track's letter, by its place among the project's returns ("" for any other track)
std::string returnLetterOf(const Project &project, int track);

// The send from a track to a return, or null
const Send *findSend(const Track &track, ObjectId to);
Send *findSend(Track &track, ObjectId to);

// Whether track `from`'s sound gets to track `to` through outputs and sends, in any number of steps
// (a track reaches itself). Only routes the rules allow are followed (a send into a return or a
// group, an output into a group).
bool reaches(const Project &project, int from, int to);
// Whether a track may go into a group (`group` -1 for the master, always allowed), or send to a
// bus (a return or a group, so one track can feed several buses): the right kind, not itself, and
// nothing that would bring its sound back to it. A send never goes to the group the track outputs
// to (it would play twice: refused), and an output never goes to a group the track sends to; and no
// send is offered past audio::kMaxSends.
bool canOutputTo(const Project &project, int track, int group);
bool canSendTo(const Project &project, int track, int bus);
// The groups a track may go into, and the buses (returns and groups) it may send to (track indices, in project order)
std::vector<int> outputChoices(const Project &project, int track);
std::vector<int> sendChoices(const Project &project, int track);
// A track's output by name: "Master", or its group's name
std::string outputName(const Project &project, int track);

// Several destinations: besides its output a track can send to groups ("Also to" in the output menus).
// The groups the menu offers: those it sends to now and those it may send to (canSendTo), in project order.
std::vector<int> alsoToChoices(const Project &project, int track);
// The groups the track sends to now (track indices, in send order; sends to returns aren't counted)
std::vector<int> groupSends(const Project &project, int track);
// Takes the send to `to` and its automation lane out of the track (nothing when there is none); one edit
void removeSend(Track &track, ObjectId to);
// Ticks or unticks "Also to" for a group: one edit adding a post-fader send at 0 dB (level 1), or taking the
// send and its lane out (nothing if the track may not, or already does as asked)
void setAlsoTo(ProjectStore &store, int track, ObjectId group, bool on);
// How many groups a track also sends to, what the strip's output label reads ("To Master", "To Group 1",
// "To Group 1 + 2": the number of groups it also sends to, returns not counted) cut to `maxChars`
// with ".." in the output group's name, and the arrangement's "Group  /  to Group 1 + 2" ending
std::string outputLabel(const Project &project, int track, size_t maxChars = 17);
std::string extraDestinations(const Project &project, int track); // "" or " + 2"
// A short name for a send bar into a group: "Group 1" is "G1", any other name its first two characters
std::string groupSendLabel(const std::string &name);
// The same for the project's group at `group`, numbered when another group's short name is the same
// ("Drums" and "Drum bus" are "Dr1" and "Dr2", in project order), so every bar reads differently
std::string groupSendLabel(const Project &project, int group);

// Folding groups in the arrangement: how many groups deep each track's output chain goes (0 for a
// track that outputs to the master; sends don't count), whether `track` reaches `group` through
// outputs alone (the group's own tracks, and the ones in groups inside it; not the group itself), and
// which tracks are hidden when the groups with the ids in `folded` are folded (1 for hidden, by
// track index). A chain that loops, which the rules refuse, ends where it comes back.
std::vector<int> groupDepth(const Project &project);
// The groups a track goes through, nearest first (its output, that group's output, ...)
std::vector<int> outputGroups(const Project &project, int track);
bool insideGroup(const Project &project, int track, int group);
std::vector<char> hiddenTracks(const Project &project, const std::set<ObjectId> &folded);
// The same for the groups the project has folded (`Track::folded`)
std::vector<char> hiddenTracks(const Project &project);

// Whether an effect of track `track` may be keyed by track `source` (a sidechain): another track, of any
// kind, that isn't reached from `track` (a sidechain is a route too, so none may loop). The master's
// effects may be keyed by any track.
bool canSidechain(const Project &project, int track, int source);

// The most keyed effects on one track, and on the master; routingError refuses more, and the device's
// Sidechain dropdown offers none once they are used up (the engine has a key buffer for each)
constexpr size_t kMaxTrackKeys = audio::kMaxKeys;
constexpr size_t kMaxMasterKeys = audio::kMaxKeys;
// Whether a device is one a sidechain may be set on: a built-in effect whose EffectInfo says sidechain, or
// any plugin (the project can't know which are installed, or have a sidechain port). The file rules, the
// key count, the routing graph and the plan use this, so a keyed plugin that is missing or has no sidechain
// port keeps its key and counts toward the limit, and the engine hands its slot the key like any other.
bool mayTakeSidechain(const audio::DeviceSetting &device);
// Whether a device actually takes one, for the Sidechain control: mayTakeSidechain, and for a plugin one
// the lookup below knows to have a sidechain port (a plugin that isn't installed or has none gets no
// control, but a key it already has is kept)
bool takesSidechain(const audio::DeviceSetting &device);
// The app tells the rules what the plugin scan found: a plugin's sidechain channels by id (0: none or
// unknown). Main thread; none set means no plugin takes one.
void setPluginSidechainLookup(std::function<uint32_t(const std::string &id)> lookup);
// How many of the effects are keyed
size_t keyedCount(const std::vector<audio::DeviceSetting> &effects);

// What a device's Sidechain control offers: whether the device takes one at all, the tracks that may key
// it (the tracks it may take, by id and name, in project order; the master's effects take every
// track), the track keying it now (kNoId: none), and, when it can't be given one, why
struct SidechainSource {
    ObjectId id = kNoId;
    std::string name;
};
struct SidechainOffer {
    bool available = false;
    std::vector<SidechainSource> sources;
    ObjectId current = kNoId;
    std::string note; // "4 sidechains per track at most": no more can be added
};
SidechainOffer sidechainOffer(const Project &project, const DevicePlace &place);

// Reordering tracks by dragging a header while some are folded away. A drag moves a block: the track
// alone, or for a folded group the group and every track hidden inside it, in project order, so they
// stay together. `slot` is where it goes among the tracks that are not in it (0 before the first of them,
// their count after the last). `settleDrop` moves a slot that would split a run of hidden tracks, or put
// the block between a folded group and its first hidden track, to the nearer end: just before the
// folded group, or just after the last track of the run. `dropOrder` is the project's track indices in
// their new order (the identity when nothing moves) and `moveBlock` applies it (the selection stays on
// its track).
std::vector<int> dragBlock(const Project &project, int track);
int settleDrop(const Project &project, const std::vector<int> &block, int slot);
std::vector<int> dropOrder(const Project &project, const std::vector<int> &block, int slot);
void moveBlock(Project &project, const std::vector<int> &block, int slot);

// A track going: the sends into it, their lanes, and the outputs into it (back to the master), and the
// sidechains from it (on every track's devices and the master's)
void forgetTrack(Project &project, ObjectId id);

// What is wrong with the project's routing, or empty: a send to something that isn't a bus (a return or
// group), to itself, twice to one, or to the group the track outputs to, more than audio::kMaxSends
// sends on a track, an output into something that isn't a group, an id no track has, a
// sidechain on a device that takes none or from no track or from its own track, a track whose sound
// comes back to it (through outputs, sends and sidechains). The file reader refuses projects with one.
std::string routingError(const Project &project);

// The graph the engine plays, worked out from the project and each track's own latency (its plugins')
struct RoutingPlan {
    struct Path {
        int to = -1;        // a track's index, or -1 for the master (an output) or nowhere (a send dropped)
        uint32_t delay = 0; // frames this path is held back, so everything meets in time
    };
    struct Node {
        int stage = 0;           // 0 for a track nothing feeds; else one more than the latest that feeds it
        Path output;             // where it goes
        std::vector<Path> sends; // one per Track::sends entry, in order (to -1: dropped)
        // The track's effects keyed by other tracks (a key that would loop is dropped): the effect's
        // index in its chain, the source track's index, and the frames the key is held back so it lines
        // up with this track's input. A key is an ordering edge (the source is in an earlier stage) and
        // a timing edge (this track's input is no earlier than the source's output), and carries no sound.
        struct Key {
            size_t effect;
            int from;
            uint32_t delay;
        };
        std::vector<Key> keys;
        bool soloed = false;     // heard while something is soloed (see planRouting)
    };
    std::vector<Node> nodes; // by track index
    std::vector<Node::Key> masterKeys; // the master's effects keyed by tracks (any track: the master runs last)
    int stages = 0;
    uint32_t masterTime = 0; // frames from the earliest sound to the master: the latest path's
};
// The plan for the project. Routes the rules refuse, or that would loop, are dropped (the output to
// the master; a send to nowhere), in track order. A track's latency delays everything it sends on;
// each path is held back (at most `maxDelay`) so all a destination takes in lines up with the
// latest of it. Solo: a track is heard while something is soloed when it is soloed, feeds a soloed
// track (a group's or a return's sources), or is fed by one of those (the returns they send to).
RoutingPlan planRouting(const Project &project, const std::vector<uint32_t> &latencies, uint32_t maxDelay);

}
