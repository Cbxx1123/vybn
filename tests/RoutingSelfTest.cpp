#include "RoutingSelfTest.h"
#include "Harness.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/AudioSystem.h"
#include "audio/Effects.h"
#include "audio/Midi.h"
#include "audio/Routing.h"
#include "audio/Synth.h"
#include "audio/Track.h"
#include "daw/audio/AudioBridge.h"
#include "daw/audio/AudioPool.h"
#include "daw/model/AutomationLanes.h"
#include "daw/widgets/Format.h"
#include "daw/model/History.h"
#include "daw/library/Library.h"
#include "daw/model/Project.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/ProjectSession.h"
#include "daw/model/ProjectStore.h"
#include "daw/model/Routing.h"
#include "daw/app/Settings.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

namespace tests {

namespace {

using daw::TrackKind;
// printf into a string
template <typename... Args>
std::string printed(const char *pattern, Args... args)
{
    char text[160];
    std::snprintf(text, sizeof text, pattern, args...);
    return text;
}

constexpr double kCentre = 0.7071067811865476; // the pan law's gain at the centre, cos(pi/4)

daw::Track trackOf(TrackKind kind, const char *name)
{
    daw::Track t;
    t.kind = kind;
    t.name = name;
    return t;
}

// An instrument track playing the built-in synth's Init sound, one note held from the start
daw::Track sourceTrack(const char *name)
{
    daw::Track t = trackOf(TrackKind::Instrument, name);
    t.instrument = audio::SynthDevice::setting();
    t.clips.push_back(daw::Clip("Held", 0.0, 16.0, {daw::Note{0.0, 16.0, 57, 0.8f}}));
    return t;
}

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

// A one-sample impulse on both channels at the start of its `at`th block (so every level has settled)
class Impulse : public audio::AudioProcessor {
public:
    explicit Impulse(int at) : AudioProcessor("Impulse"), at_(at) {}
    void process(audio::AudioBlock &block) override
    {
        if (blocks_++ == at_)
            for (uint32_t c = 0; c < block.channelCount; ++c)
                block.channels[c][0] += 1.0f;
    }

private:
    int at_;
    int blocks_ = 0;
};

bool near(double a, double b, double tolerance = 1e-4)
{
    return std::fabs(a - b) <= tolerance;
}

// The project's rules: who may go where, names, deleting, the file, undo
void modelChecks(Checker &c)
{
    daw::Project p;
    p.tracks = {trackOf(TrackKind::Audio, "A"),       trackOf(TrackKind::Instrument, "B"), trackOf(TrackKind::Return, "R1"),
                trackOf(TrackKind::Return, "R2"),     trackOf(TrackKind::Group, "G1"),     trackOf(TrackKind::Group, "G2")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId r1 = p.tracks[2].id, r2 = p.tracks[3].id, g1 = p.tracks[4].id, g2 = p.tracks[5].id;

    c.check(daw::returnLetter(0) == "A" && daw::returnLetter(25) == "Z" && daw::returnLetter(26) == "AA" &&
                daw::returnLetter(27) == "AB" && daw::returnLetterOf(p, 3) == "B" && daw::returnLetterOf(p, 0).empty(),
            "Routing: returns are lettered A, B, ... Z, AA");
    {
        daw::Project named;
        named.addTrack(TrackKind::Return);
        named.addTrack(TrackKind::Group);
        named.addTrack(TrackKind::Return);
        c.check(named.tracks[0].name == "Return A" && named.tracks[1].name == "Group 1" && named.tracks[2].name == "Return B",
                "Routing: new returns and groups are named \"Return A\", \"Group 1\"",
                named.tracks[0].name + ", " + named.tracks[1].name + ", " + named.tracks[2].name);
    }

    // Choices: groups for the output, returns for sends, nothing that would loop
    c.check(daw::outputChoices(p, 0) == std::vector<int>{4, 5} && daw::sendChoices(p, 0) == std::vector<int>{2, 3, 4, 5},
            "Routing: a track may go into any group and send to any return or group");
    p.tracks[4].output = g2; // G1 into G2
    p.tracks[2].output = g1; // R1 into G1
    c.check(daw::reaches(p, 2, 5) && !daw::reaches(p, 5, 2) && daw::reaches(p, 1, 1),
            "Routing: reaches follows outputs through groups (and a track reaches itself)");
    c.check(!daw::canOutputTo(p, 5, 4) && daw::outputChoices(p, 5).empty() && !daw::canOutputTo(p, 4, 4),
            "Routing: a group can't go into one it feeds, or itself");
    c.check(!daw::canSendTo(p, 4, 2) && daw::canSendTo(p, 4, 3) && !daw::canSendTo(p, 2, 2) && daw::canSendTo(p, 0, 4) &&
                !daw::canSendTo(p, 0, 1) && !daw::canSendTo(p, 5, 4),
            "Routing: no send into a bus that feeds the sender, to itself, or to a track that isn't a bus; a send to a group is fine");
    c.check(daw::outputName(p, 2) == "G1" && daw::outputName(p, 0) == "Master", "Routing: outputs are named by their group");

    // The rules the file reader holds a project to
    c.check(daw::routingError(p).empty(), "Routing: a sound routing has no error", daw::routingError(p));
    auto broken = [&p](auto change) {
        daw::Project q = p;
        change(q);
        return daw::routingError(q);
    };
    const std::string toGroup = broken([&](daw::Project &q) { q.tracks[0].sends.push_back({p.tracks[1].id, 0.5f, false}); });
    const std::string toReturn = broken([&](daw::Project &q) { q.tracks[0].output = r1; });
    const std::string unknown = broken([&](daw::Project &q) { q.tracks[0].output = 999; });
    const std::string twice = broken([&](daw::Project &q) { q.tracks[0].sends = {{r1, 0.5f, false}, {r1, 0.2f, true}}; });
    const std::string loop = broken([&](daw::Project &q) { q.tracks[5].sends.push_back({r1, 1.0f, false}); });
    c.check(toGroup.find("isn't a bus") != std::string::npos && toReturn.find("isn't a group") != std::string::npos &&
                unknown.find("no track has") != std::string::npos && twice.find("two sends") != std::string::npos &&
                loop.find("comes back") != std::string::npos,
            "Routing: wrong sends, outputs, ids and loops are named", loop);

    // The file: kinds, outputs, sends, a send's lane and the master volume read back exactly
    p.tracks[0].sends = {{r1, 0.5f, false}, {r2, 0.25f, true}};
    p.tracks[0].output = g1;
    p.tracks[1].sends = {{r2, 1.0f, false}};
    p.tracks[0].automation.push_back({daw::sendParam(r1), {{0.0, 0.25f}, {8.0, 0.75f}}});
    p.masterVolume = 0.5f;
    const std::string text = daw::writeProject(p);
    daw::Project back;
    std::string why;
    const bool read = daw::readProject(text, back, why);
    c.check(read && daw::sameContent(p, back) && back.tracks[0].sends == p.tracks[0].sends && back.tracks[0].output == g1 &&
                back.tracks[2].kind == TrackKind::Return && back.tracks[4].kind == TrackKind::Group &&
                back.masterVolume == 0.5f && daw::writeProject(back) == text,
            "Routing: the file round-trips kinds, outputs, sends, send lanes and the master volume", why);
    auto refused = [](const std::string &file) {
        daw::Project q;
        std::string reason;
        return daw::readProject(file, q, reason) ? std::string() : reason;
    };
    const std::string head = "format = 3\n[[tracks]]\nid = 1\nname = \"A\"\n";
    const std::string sendToGroup = refused(head + "sends = [ { to = 2, level = 0.5 } ]\n[[tracks]]\nid = 2\nname = \"B\"\n");
    const std::string outToNothing = refused(head + "output = 7\n");
    const std::string badLane = refused(head + "automation = [ { param = \"send\", points = [] } ]\n");
    c.check(sendToGroup.find("isn't a bus") != std::string::npos && outToNothing.find("no track has") != std::string::npos &&
                badLane.find("to") != std::string::npos,
            "Routing: files with wrong routing are refused with a reason", sendToGroup);
    {
        daw::Project q;
        const bool ok = daw::readProject("format = 3\nmaster_volume = 9\n[[tracks]]\nname = \"R\"\nkind = \"return\"\narmed = true\n"
                                         "sends = []\n[[tracks.clips]]\nname = \"x\"\n",
                                         q, why);
        c.check(ok && q.masterVolume == 2.0f && !q.tracks.empty() && !q.tracks[0].armed && q.tracks[0].clips.empty(),
                "Routing: the master volume is clamped, buses neither arm nor hold clips", why);
    }

    // Deleting a return takes the sends into it and their lanes; deleting a group sends its tracks to the master
    {
        daw::Project q = p;
        q.deleteTrack(2); // R1
        const bool sendsGone = daw::findSend(q.tracks[0], r1) == nullptr && daw::findSend(q.tracks[0], r2) != nullptr &&
                               audio::findAutomation(q.tracks[0].automation, daw::sendParam(r1)) == nullptr;
        q.deleteTrack(3); // G1 (moved up)
        c.check(sendsGone && q.tracks[0].output == daw::kNoId && daw::routingError(q).empty(),
                "Routing: deleting a return or a group takes what pointed at it");
    }

    // Undo: each routing edit is a step with its name, put back through the mix (no track's content moves)
    daw::ProjectStore store(p);
    daw::History history;
    history.reset(store.project());
    double now = 1.0;
    auto step = [&](auto change) {
        change();
        history.update(store, true, false, now);
        now += 2.0;
        return history.undoLabel();
    };
    const std::string changed = step([&] { store.editMix(1).sends[0].level = 0.3f; });
    const std::string added = step([&] { store.editMix(1).sends.push_back({r1, 0.7f, false}); });
    const std::string removed = step([&] { store.editMix(1).sends.pop_back(); });
    const std::string routed = step([&] { store.editMix(1).output = g2; });
    const std::string master = step([&] { store.editMasterVolume() = 0.25f; });
    c.check(changed == "Change send" && added == "Add send" && removed == "Remove send" && routed == "Route track" &&
                master == "Master volume",
            "Routing: undo steps are named", changed + ", " + added + ", " + removed + ", " + routed + ", " + master);
    const uint64_t content = store.trackRevision(1), masterRevision = store.masterRevision();
    daw::HistoryRestore plan;
    bool undone = history.undo(store, now, plan);
    store.restore(std::move(plan));
    c.check(undone && store.project().masterVolume == 0.5f && store.masterRevision() != masterRevision,
            "Routing: undoing the master volume puts it back");
    const uint64_t mix = store.mixRevision(1);
    undone = history.undo(store, now, plan);
    store.restore(std::move(plan));
    c.check(undone && store.project().tracks[1].output == daw::kNoId && store.mixRevision(1) != mix &&
                store.trackRevision(1) == content,
            "Routing: undoing a route puts it back through the mix alone");
    {
        // A return deleted and brought back by undo takes its sends with it and back
        daw::ProjectStore s2(p);
        daw::History h2;
        h2.reset(s2.project());
        s2.edit().deleteTrack(2);
        h2.update(s2, true, false, 1.0);
        daw::HistoryRestore restore;
        const bool ok = h2.undo(s2, 3.0, restore);
        s2.restore(std::move(restore));
        c.check(ok && h2.redoLabel() == "Delete track" && s2.project().tracks[0].sends == p.tracks[0].sends &&
                    audio::findAutomation(s2.project().tracks[0].automation, daw::sendParam(r1)) != nullptr,
                "Routing: undoing a return's delete brings its sends and lanes back");
    }
}

// Folding groups: how deep each track's output chain goes, and which tracks a folded group hides
void foldChecks(Checker &c)
{
    daw::Project p;
    // 0 Outer (group, to the master), 1 Inner (group, into Outer), 2 Kick (into Inner), 3 Pad (into Outer),
    // 4 Lead (to the master, sends to Verb), 5 Verb (return)
    p.tracks = {trackOf(TrackKind::Group, "Outer"), trackOf(TrackKind::Group, "Inner"), trackOf(TrackKind::Audio, "Kick"),
                trackOf(TrackKind::Audio, "Pad"),   trackOf(TrackKind::Audio, "Lead"),  trackOf(TrackKind::Return, "Verb")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId outer = p.tracks[0].id, inner = p.tracks[1].id, verb = p.tracks[5].id;
    p.tracks[1].output = outer;
    p.tracks[2].output = inner;
    p.tracks[3].output = outer;
    p.tracks[4].sends.push_back({verb, 0.5f});

    const std::vector<int> depth = daw::groupDepth(p);
    c.check(depth == std::vector<int>{0, 1, 2, 1, 0, 0}, "Folding: depth counts the groups a track's outputs go through (sends don't)",
            printed("%d %d %d %d %d %d", depth[0], depth[1], depth[2], depth[3], depth[4], depth[5]));
    c.check(daw::insideGroup(p, 2, 1) && daw::insideGroup(p, 2, 0) && !daw::insideGroup(p, 3, 1) && !daw::insideGroup(p, 0, 0) &&
                !daw::insideGroup(p, 1, 2) && !daw::insideGroup(p, 4, 0) && !daw::insideGroup(p, -1, 0),
            "Folding: a track is inside the groups its output chain reaches, not itself or a group below it");

    auto hiddenOf = [&p](std::set<daw::ObjectId> folded) { return daw::hiddenTracks(p, folded); };
    c.check(hiddenOf({}) == std::vector<char>(6, 0), "Folding: nothing is hidden while no group is folded");
    c.check(hiddenOf({inner}) == std::vector<char>{0, 0, 1, 0, 0, 0}, "Folding: the inner group hides only its own track");
    c.check(hiddenOf({outer}) == std::vector<char>{0, 1, 1, 1, 0, 0},
            "Folding: the outer group hides the inner group and its tracks");
    c.check(hiddenOf({outer, inner}) == std::vector<char>{0, 1, 1, 1, 0, 0}, "Folding: both folded hide no more than the outer");
    c.check(hiddenOf({verb, p.tracks[4].id}) == std::vector<char>(6, 0),
            "Folding: a track outputting to the master isn't hidden, and sends don't hide (a return in the set hides nothing)");

    // A loop (the rules refuse it, but a bad project must not hang the view)
    p.tracks[0].output = inner;
    const std::vector<int> looped = daw::groupDepth(p);
    const std::vector<char> hiddenLooped = hiddenOf({outer});
    c.check(looped.size() == 6 && hiddenLooped.size() == 6 && !daw::insideGroup(p, 0, 0),
            "Folding: a loop in the outputs ends instead of hanging");
}

// The fold state is the project's: saved, not an undo step, kept through undo; and dragging tracks while
// groups are folded (dragBlock, settleDrop, dropOrder, moveBlock)
void foldProjectChecks(Checker &c)
{
    // The same project as foldChecks: 0 Outer (group), 1 Inner (group, into Outer), 2 Kick (into Inner),
    // 3 Pad (into Outer), 4 Lead (to the master), 5 Verb (return)
    auto make = [] {
        daw::Project p;
        p.tracks = {trackOf(TrackKind::Group, "Outer"), trackOf(TrackKind::Group, "Inner"), trackOf(TrackKind::Audio, "Kick"),
                    trackOf(TrackKind::Audio, "Pad"),   trackOf(TrackKind::Audio, "Lead"),  trackOf(TrackKind::Return, "Verb")};
        daw::ObjectId next = 1;
        p.normalizeIds(next);
        p.tracks[1].output = p.tracks[0].id;
        p.tracks[2].output = p.tracks[1].id;
        p.tracks[3].output = p.tracks[0].id;
        return p;
    };

    // The file: a folded group is saved and read back; a folded flag on anything but a group is ignored
    {
        daw::Project p = make();
        p.tracks[1].folded = true;
        const std::string text = daw::writeProject(p);
        daw::Project q;
        std::string why;
        const bool ok = daw::readProject(text, q, why);
        std::string unfoldedText = daw::writeProject(make());
        daw::Project r;
        const bool ok2 = daw::readProject(unfoldedText, r, why);
        c.check(ok && q.tracks.size() == 6 && q.tracks[1].folded && !q.tracks[0].folded && text.find("folded = true") != std::string::npos &&
                    ok2 && !r.tracks[1].folded && unfoldedText.find("folded") == std::string::npos,
                "Folding: a folded group is saved (only then) and read back", why);
        daw::Project s;
        const bool ok3 = daw::readProject("format = 3\n[[tracks]]\nname = \"A\"\nkind = \"audio\"\nfolded = true\n"
                                         "[[tracks]]\nname = \"G\"\nkind = \"group\"\nfolded = true\n",
                                         s, why);
        c.check(ok3 && s.tracks.size() == 2 && !s.tracks[0].folded && s.tracks[1].folded,
                "Folding: folded on a track that isn't a group is ignored", why);
        c.check(daw::hiddenTracks(q) == std::vector<char>{0, 0, 1, 0, 0, 0},
                "Folding: a project read folded hides the tracks inside the folded group");
    }

    // Not an undo step, but a change to what is saved: the project reads as modified
    {
        daw::ProjectSession session(make());
        session.history().reset(session.project());
        session.recordHistory(true, false, 0.0);
        session.checkModified();
        const bool before = session.modified();
        session.store().editMix(0).folded = true;
        session.recordHistory(true, false, 1.0);
        session.checkModified();
        c.check(!before && !session.history().canUndo() && session.modified(),
                "Folding: folding is not an undo step, but marks the project modified");
        session.store().editMix(0).folded = false;
        session.checkModified();
        c.check(!session.modified(), "Folding: folded back to what was saved, the project isn't modified");
    }

    // Undo of something else keeps the fold as it is now; undoing a group's delete brings it back as it was
    {
        daw::ProjectStore store(make());
        daw::History history;
        history.reset(store.project());
        store.edit().deleteTrack(5); // the return: a structural edit
        history.update(store, true, false, 1.0);
        store.editMix(0).folded = true;
        daw::HistoryRestore plan;
        const bool undone = history.undo(store, 3.0, plan);
        store.restore(std::move(plan));
        c.check(undone && store.project().tracks.size() == 6 && store.project().tracks[0].folded && !store.project().tracks[1].folded,
                "Folding: undoing a structural edit keeps the fold as it is now");

        daw::ProjectStore store2(make());
        store2.editMix(0).folded = true;
        daw::History history2;
        history2.reset(store2.project());
        store2.edit().deleteTrack(0); // the folded group goes
        history2.update(store2, true, false, 1.0);
        daw::HistoryRestore plan2;
        const bool undone2 = history2.undo(store2, 3.0, plan2);
        store2.restore(std::move(plan2));
        c.check(undone2 && store2.project().tracks.size() == 6 && store2.project().tracks[0].name == "Outer" &&
                    store2.project().tracks[0].folded,
                "Folding: undoing the delete of a folded group brings it back folded");
    }

    // Dragging: a folded group's block, and where a drop may land
    {
        daw::Project p = make();
        // Nothing folded: a block is the track alone, and no slot is moved
        const std::vector<int> single = daw::dragBlock(p, 4);
        c.check(single == std::vector<int>{4} && daw::dragBlock(p, 0) == std::vector<int>{0} && daw::dragBlock(p, 9).empty() &&
                    daw::settleDrop(p, single, 2) == 2 && daw::settleDrop(p, single, 0) == 0,
                "Folding: with nothing folded a drag is the track alone and its drop isn't moved");

        p.tracks[0].folded = true; // Outer: hides Inner, Kick and Pad
        const std::vector<int> lead{4};
        // The rest is Outer, Inner, Kick, Pad, Verb; a slot before a hidden track goes to the nearer end
        const int s0 = daw::settleDrop(p, lead, 0), s1 = daw::settleDrop(p, lead, 1), s2 = daw::settleDrop(p, lead, 2),
                  s3 = daw::settleDrop(p, lead, 3), s4 = daw::settleDrop(p, lead, 4), s5 = daw::settleDrop(p, lead, 5);
        c.check(s0 == 0 && s1 == 0 && s2 == 4 && s3 == 4 && s4 == 4 && s5 == 5,
                "Folding: a drop between a folded group and its hidden tracks, or among them, goes before the group or after the last",
                printed("%d %d %d %d %d %d", s0, s1, s2, s3, s4, s5));
        // The track lands where the slot says
        c.check(daw::dropOrder(p, lead, s1) == std::vector<int>{4, 0, 1, 2, 3, 5} &&
                    daw::dropOrder(p, lead, s3) == std::vector<int>{0, 1, 2, 3, 4, 5},
                "Folding: a dragged track lands before the folded group or after its last hidden track");

        // A group folded inside an open one (Inner folded in Outer: Kick is hidden)
        daw::Project q = make();
        q.tracks[1].folded = true;
        const int t1 = daw::settleDrop(q, lead, 2); // rest: Outer, Inner, Kick, Pad, Verb: before Kick
        const int t2 = daw::settleDrop(q, lead, 3); // before Pad (shows)
        c.check(t1 == 3 && t2 == 3, "Folding: a drop between a folded inner group and its hidden track lands outside the run",
                printed("%d %d", t1, t2));

        // A folded group moves with the tracks hidden inside it, in order, right after it
        const std::vector<int> block = daw::dragBlock(p, 0);
        c.check(block == std::vector<int>{0, 1, 2, 3}, "Folding: dragging a folded group takes its hidden tracks with it");
        c.check(daw::dropOrder(p, block, 2) == std::vector<int>{4, 5, 0, 1, 2, 3} &&
                    daw::dropOrder(p, block, 1) == std::vector<int>{4, 0, 1, 2, 3, 5} &&
                    daw::dropOrder(p, block, 0) == std::vector<int>{0, 1, 2, 3, 4, 5},
                "Folding: the block lands whole in each slot among the other tracks");
        p.select(2); // Kick
        const daw::ObjectId kick = p.tracks[2].id;
        daw::moveBlock(p, block, 2);
        c.check(p.tracks[0].name == "Lead" && p.tracks[1].name == "Verb" && p.tracks[2].name == "Outer" &&
                    p.tracks[3].name == "Inner" && p.tracks[4].name == "Kick" && p.tracks[5].name == "Pad" &&
                    p.selectedTrack == 4 && p.tracks[static_cast<size_t>(p.selectedTrack)].id == kick && p.tracks[2].folded,
                "Folding: moving a folded group moves its tracks with it, the selection staying on its track");
        // A folded group dropped among another's hidden tracks lands outside them
        daw::Project r = make();
        r.tracks[0].folded = true;
        r.tracks.push_back(trackOf(TrackKind::Group, "Other"));
        daw::ObjectId next = 100;
        r.normalizeIds(next);
        r.tracks[6].folded = true;
        const std::vector<int> otherBlock = daw::dragBlock(r, 6);
        // rest: Outer, Inner, Kick, Pad, Lead, Verb: slot 2 is before Kick, among Outer's hidden tracks
        const int landed = daw::settleDrop(r, otherBlock, 2);
        c.check(otherBlock == std::vector<int>{6} && landed == 4,
                "Folding: a folded group dropped among another's hidden tracks lands outside them", std::to_string(landed));
    }
}

// planRouting: stages, delays lining everything up at the master, loops dropped, solo
void planChecks(Checker &c)
{
    daw::Project p;
    p.tracks = {trackOf(TrackKind::Audio, "A"), trackOf(TrackKind::Audio, "B"), trackOf(TrackKind::Group, "G1"),
                trackOf(TrackKind::Return, "R1"), trackOf(TrackKind::Group, "G2")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId g1 = p.tracks[2].id, r1 = p.tracks[3].id, g2 = p.tracks[4].id;
    p.tracks[0].output = g1;            // A -> G1 -> G2 -> master
    p.tracks[2].output = g2;
    p.tracks[1].sends = {{r1, 1.0f, false}}; // B -> master, and a send to R1 -> master
    // Latencies: A 100, B 0, G1 50, R1 30, G2 0
    const daw::RoutingPlan plan = daw::planRouting(p, {100, 0, 50, 30, 0}, 16384);
    const auto &n = plan.nodes;
    c.check(plan.stages == 3 && n[0].stage == 0 && n[1].stage == 0 && n[2].stage == 1 && n[3].stage == 1 && n[4].stage == 2,
            "Routing plan: a bus is a stage after everything that feeds it");
    c.check(n[0].output.to == 2 && n[2].output.to == 4 && n[4].output.to == -1 && n[1].sends.size() == 1 && n[1].sends[0].to == 3,
            "Routing plan: outputs and sends point where the project says");
    // The latest path is A's: 100 + 50 = 150 frames. B waits 150 at the master and its send none at R1 (R1's
    // only source); R1's own output waits 120 (150 - 30); A's path through the groups waits nothing.
    c.check(plan.masterTime == 150 && n[0].output.delay == 0 && n[2].output.delay == 0 && n[4].output.delay == 0 &&
                n[1].output.delay == 150 && n[1].sends[0].delay == 0 && n[3].output.delay == 120,
            "Routing plan: each path is held back to meet the latest at the master",
            "B " + std::to_string(n[1].output.delay) + ", R1 " + std::to_string(n[3].output.delay));
    // Two sources into one group, one later than the other: the earlier is held back to meet it there
    {
        daw::Project q = p;
        q.tracks[1].output = g1;
        const daw::RoutingPlan two = daw::planRouting(q, {100, 0, 50, 30, 0}, 16384);
        c.check(two.nodes[1].output.delay == 100 && two.nodes[0].output.delay == 0 && two.nodes[1].sends[0].delay == 0,
                "Routing plan: sources into one group line up there, a send keeps its own delay");
        const daw::RoutingPlan capped = daw::planRouting(q, {100, 0, 50, 30, 0}, 64);
        c.check(capped.nodes[1].output.delay == 64, "Routing plan: delays are capped");
    }
    // A loop (G2 back into G1) is dropped: the later route goes to the master
    {
        daw::Project q = p;
        q.tracks[4].output = g1;
        const daw::RoutingPlan looped = daw::planRouting(q, {0, 0, 0, 0, 0}, 16384);
        c.check(looped.nodes[2].output.to == 4 && looped.nodes[4].output.to == -1 && looped.stages == 3,
                "Routing plan: a route that would loop is dropped");
    }
    // Solo: a soloed track's group, and its return, are heard; a soloed group's sources are heard
    {
        daw::Project q = p;
        q.tracks[1].solo = true;
        const daw::RoutingPlan s1 = daw::planRouting(q, {}, 16384);
        c.check(s1.nodes[1].soloed && s1.nodes[3].soloed && !s1.nodes[0].soloed && !s1.nodes[2].soloed && !s1.nodes[4].soloed,
                "Routing plan: soloing a track keeps the returns it sends to");
        q.tracks[1].solo = false;
        q.tracks[2].solo = true;
        const daw::RoutingPlan s2 = daw::planRouting(q, {}, 16384);
        c.check(s2.nodes[0].soloed && s2.nodes[2].soloed && s2.nodes[4].soloed && !s2.nodes[1].soloed && !s2.nodes[3].soloed,
                "Routing plan: soloing a group keeps what feeds it and where it goes");
        q.tracks[2].solo = false;
        const daw::RoutingPlan none = daw::planRouting(q, {}, 16384);
        c.check(!none.nodes[0].soloed && !none.nodes[3].soloed, "Routing plan: with nothing soloed nothing is marked");
    }
}

// The engine with a hand-made routing: an impulse through a track, with a post-fader send held back 100
// frames and a pre-fader send held back 50, both into a return
void impulseChecks(Checker &c, env::Environment &environment)
{
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    auto group = std::make_shared<audio::SoloGroup>();
    constexpr int kAt = 60; // blocks of 512 before the impulse: every level has glided home
    audio::TrackProcessor &track = engine.add<audio::TrackProcessor>("T", group, std::make_unique<Impulse>(kAt));
    audio::TrackProcessor &ret = engine.add<audio::TrackProcessor>("R", group);
    track.gain.set(0.5f);
    track.setSendLevel(0, 0.5f);
    track.setSendLevel(1, 0.25f);
    auto routing = std::make_unique<audio::Routing>();
    routing->beginStage();
    routing->add({&track, nullptr, {{&ret, 0, 0, 100, false}, {&ret, 1, 0, 50, true}}, {}, {}});
    routing->beginStage();
    routing->add({&ret, nullptr, {}, {}, {}});
    engine.setRouting(std::move(routing));

    std::vector<float> out(512 * 2);
    auto toImpulse = [&] {
        for (int b = 0; b <= kAt; ++b)
            engine.renderOffline(out.data(), 512, 48000.0);
    };
    toImpulse();
    // The track: 0.5 at the centre; the pre send: 0.25 of the dry impulse into R, then R's centre; the
    // post send: 0.5 of the track's output (0.5 x centre), then R's centre
    const double k = kCentre;
    const double at0 = out[0], at50 = out[100], at100 = out[200], between = out[150];
    c.check(near(at0, 0.5 * k) && near(at50, 0.25 * k) && near(at100, 0.5 * 0.5 * k * k) && near(between, 0.0),
            "Routing engine: post- and pre-fader sends at their own delays, into a return",
            printed("%.4f %.4f %.4f", at0, at50, at100));
    const double right = out[201];
    c.check(near(right, at100), "Routing engine: both channels alike");

    // Muted: the output and both sends are silent
    audio::TrackProcessor &muted = engine.add<audio::TrackProcessor>("M", group, std::make_unique<Impulse>(kAt));
    muted.mute = true;
    muted.setSendLevel(0, 1.0f);
    routing = std::make_unique<audio::Routing>();
    routing->beginStage();
    routing->add({&muted, nullptr, {{&ret, 0, 0, 0, true}}, {}, {}});
    routing->beginStage();
    routing->add({&ret, nullptr, {}, {}, {}});
    engine.setRouting(std::move(routing));
    toImpulse();
    float peak = 0.0f;
    for (float v : out)
        peak = std::max(peak, std::fabs(v));
    c.check(peak < 1e-6f, "Routing engine: a muted track's pre-fader send is silent too");

    // A processor a routing names being removed drops the routing (a safety), and nothing is read after
    engine.remove(muted);
    engine.renderOffline(out.data(), 512, 48000.0);
    c.check(engine.processorCount() == 2, "Routing engine: removing a routed processor is safe");
    engine.setRouting(nullptr);
}

// Through the bridge, with the built-in synth holding a note: levels against the same track alone
void bridgeChecks(Checker &c, env::Environment &environment)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "vibin-routing-selftest").u8string();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    daw::Settings settings;
    settings.libraryPath = dir;
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    audio::MidiHub &midi = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
    daw::AudioBridge::SyncOptions options;
    options.listen = false;

    daw::Project p;
    p.tracks = {sourceTrack("Source"), trackOf(TrackKind::Return, "Return A"), trackOf(TrackKind::Group, "Group 1"),
                trackOf(TrackKind::Group, "Group 2"), sourceTrack("Other")};
    p.tracks[4].mute = true; // heard only where a check says
    daw::ProjectStore store(p);
    bridge.sync(store, options);
    const daw::ObjectId ret = store.project().tracks[1].id, g1 = store.project().tracks[2].id, g2 = store.project().tracks[3].id;
    auto level = [&] {
        bridge.sync(store, options);
        return rmsOf(playFromStart(engine, 2.0), 24000, 48000);
    };
    const double alone = level();
    const uint64_t made = bridge.processorsMade(), removed = bridge.processorsRemoved();
    auto ratio = [&](const char *what, double wanted, const std::string &detail = {}) {
        const double got = level() / std::max(alone, 1e-12);
        c.check(alone > 1e-3 && near(got, wanted, 0.005 * wanted + 1e-6), what,
                printed("%.4f of the track alone (wanted %.4f)", got, wanted) + detail);
    };
    const double k = kCentre;

    store.editMix(0).sends = {{ret, 0.5f, false}};
    ratio("Routing: a post-fader send at half adds the return (after both pans)", 1.0 + 0.5 * k);
    store.editMix(1).mute = true;
    ratio("Routing: a muted return adds nothing", 1.0);
    store.editMix(1).mute = false;
    store.editMix(0).sends = {{ret, 0.5f, true}};
    store.editMix(0).gain = 0.0f;
    ratio("Routing: a pre-fader send plays with the fader down", 0.5);
    store.editMix(0).mute = true;
    ratio("Routing: muting the track silences its pre-fader send", 0.0);
    store.editMix(0).mute = false;
    store.editMix(0).gain = 1.0f;

    // Send automation: the lane (on the fader's taper, 0 dB at most) in place of the send's own level
    store.editMix(0).sends = {{ret, 0.0f, false}};
    store.editTrack(0).automation.push_back({daw::sendParam(ret), {{0.0, static_cast<float>(std::cbrt(0.5))}}});
    ratio("Routing: a send follows its automation lane", 1.0 + 0.5 * k);
    store.editTrack(0).automation.clear();
    store.editMix(0).sends.clear();

    // Groups: the track into Group 1 at half; a Gain effect on the group applies once to the sum
    store.editMix(0).output = g1;
    store.editMix(2).gain = 0.5f;
    ratio("Routing: a group's fader plays what goes into it (after its pan)", 0.5 * k);
    store.editMix(2).gain = 1.0f;
    {
        audio::DeviceSetting gain = audio::effectSetting(audio::EffectType::Gain);
        gain.setValue(0, 20.0 * std::log10(0.5));
        store.editTrack(2).effects.push_back(gain);
    }
    ratio("Routing: a group's effect plays on what goes into it", 0.5 * k);
    store.editTrack(2).effects.clear();
    store.editMix(2).output = g2;
    ratio("Routing: groups go into groups", k * k);

    // Solo: the soloed track's groups play; another track stays silent; soloing a group plays its sources
    store.editMix(4).mute = false;
    store.editMix(0).solo = true;
    bridge.sync(store, options);
    c.check(bridge.processor(2)->audible() && bridge.processor(3)->audible() && !bridge.processor(4)->audible() &&
                !bridge.processor(1)->audible(),
            "Routing: a soloed track's groups are heard, others aren't");
    store.editMix(0).solo = false;
    store.editMix(3).solo = true;
    bridge.sync(store, options);
    c.check(bridge.processor(0)->audible() && bridge.processor(2)->audible() && !bridge.processor(4)->audible(),
            "Routing: soloing a group plays what feeds it");
    store.editMix(3).solo = false;
    store.editMix(4).mute = true;
    store.editMix(0).output = daw::kNoId;

    // The master volume, after everything
    store.editMasterVolume() = 0.5f;
    ratio("Routing: the master volume scales the mix", 0.5);
    store.editMasterVolume() = 1.0f;

    c.check(bridge.processorsMade() == made && bridge.processorsRemoved() == removed,
            "Routing: routing edits make and remove no processor");

    // Deleting a return the track sends to: the routing is replaced before its processor goes
    store.editMix(0).sends = {{ret, 1.0f, false}};
    bridge.sync(store, options);
    store.edit().deleteTrack(1);
    const double after = level();
    c.check(bridge.processorsRemoved() == removed + 1 && near(after / alone, 1.0, 0.005),
            "Routing: deleting a return takes its share out of the mix");
    bridge.dropAll();
}

// Flexible routing: sends into groups as well as returns, and no cap of 16 sends
void flexibleModelChecks(Checker &c)
{
    daw::Project p;
    p.tracks = {trackOf(TrackKind::Audio, "A"), trackOf(TrackKind::Audio, "B"), trackOf(TrackKind::Group, "G1"),
                trackOf(TrackKind::Group, "G2"), trackOf(TrackKind::Group, "G3"), trackOf(TrackKind::Return, "R")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId g1 = p.tracks[2].id, g2 = p.tracks[3].id, g3 = p.tracks[4].id, r = p.tracks[5].id;

    // A group is a bus a track can send to, besides the group it goes into: that one is refused (it would play twice)
    p.tracks[0].output = g1;
    c.check(daw::canSendTo(p, 0, 3) && daw::canSendTo(p, 0, 5) && !daw::canSendTo(p, 0, 2) && !daw::canSendTo(p, 0, 0) &&
                !daw::canSendTo(p, 0, 1) && daw::sendChoices(p, 0) == std::vector<int>{3, 4, 5},
            "Flexible routing: a track may send to a group, but not to the one it outputs to");
    p.tracks[0].sends = {{g2, 0.5f, false}};
    c.check(daw::canOutputTo(p, 0, 2) && !daw::canOutputTo(p, 0, 3) && daw::outputChoices(p, 0) == std::vector<int>{2, 4} &&
                daw::sendChoices(p, 0) == std::vector<int>{3, 4, 5} && daw::routingError(p).empty(),
            "Flexible routing: a group a track sends to isn't offered as its output (and its send stays offered)",
            daw::routingError(p));

    // A track may feed several buses: an output, sends into groups and a return, even one reached by another path
    p.tracks[0].sends = {{g2, 0.5f, false}, {g3, 0.25f, true}, {r, 1.0f, false}};
    p.tracks[2].output = g3; // G1 into G3, so A reaches G3 twice (through its output's chain and its send): allowed
    c.check(daw::routingError(p).empty() && daw::reaches(p, 0, 4) && daw::canSendTo(p, 1, 4),
            "Flexible routing: one track may feed several buses, even one reached by another path", daw::routingError(p));

    // Loops through a send into a group are refused: G3 can't send to a group that feeds it
    p.tracks[3].output = g3; // G2 into G3
    c.check(!daw::canSendTo(p, 4, 3) && !daw::canSendTo(p, 4, 2) && daw::canSendTo(p, 3, 5) && daw::routingError(p).empty(),
            "Flexible routing: a group can't send into a group that feeds it");
    auto broken = [&p](auto change) {
        daw::Project q = p;
        change(q);
        return daw::routingError(q);
    };
    const std::string loop = broken([&](daw::Project &q) { q.tracks[4].sends = {{g2, 1.0f, false}}; });
    const std::string selfSend = broken([&](daw::Project &q) { q.tracks[2].sends = {{g1, 1.0f, false}}; });
    const std::string ownOutput = broken([&](daw::Project &q) {
        q.tracks[1].output = g1;
        q.tracks[1].sends = {{g1, 0.5f, false}};
    });
    const std::string notBus = broken([&](daw::Project &q) { q.tracks[0].sends.push_back({q.tracks[1].id, 0.5f, false}); });
    const std::string twice = broken([&](daw::Project &q) { q.tracks[1].sends = {{g2, 0.5f, false}, {g2, 0.2f, true}}; });
    c.check(loop.find("comes back") != std::string::npos && selfSend.find("to itself") != std::string::npos &&
                ownOutput.find("also its output") != std::string::npos && notBus.find("isn't a bus") != std::string::npos &&
                twice.find("two sends") != std::string::npos,
            "Flexible routing: a loop through a send into a group, a send to itself or to its own output group, to a plain track, "
            "or twice to one bus are refused with a reason",
            loop + " | " + ownOutput + " | " + notBus);

    // The file: a send into a group, with its lane, round trips; the same refusals when read
    p.tracks[0].automation.push_back({daw::sendParam(g2), {{0.0, 0.25f}, {8.0, 0.75f}}});
    const std::string text = daw::writeProject(p);
    daw::Project back;
    std::string why;
    const bool read = daw::readProject(text, back, why);
    c.check(read && daw::sameContent(p, back) && daw::sameSaved(p, back) && back.tracks[0].sends == p.tracks[0].sends &&
                back.tracks[0].sends[0].to == g2 && back.tracks[0].output == g1 && daw::writeProject(back) == text,
            "Flexible routing: the file round-trips sends into groups and a send's lane to one", why);
    auto refused = [](const std::string &file) {
        daw::Project q;
        std::string reason;
        return daw::readProject(file, q, reason) ? std::string("(read)") : reason;
    };
    const std::string head = "format = 3\n[[tracks]]\nid = 1\nname = \"A\"\noutput = 2\n";
    const std::string group = "[[tracks]]\nid = 2\nname = \"G\"\nkind = \"group\"\n";
    const std::string sendOk =
        refused(head + "sends = [ { to = 3, level = 0.5 } ]\n" + group + "[[tracks]]\nid = 3\nname = \"H\"\nkind = \"group\"\n");
    const std::string sendOwn = refused(head + "sends = [ { to = 2, level = 0.5 } ]\n" + group);
    const std::string sendToTrack = refused(head + "sends = []\n" + group + "sends = [ { to = 1 } ]\n");
    c.check(sendOk == "(read)" && sendOwn.find("also its output") != std::string::npos &&
                sendToTrack.find("isn't a bus") != std::string::npos,
            "Flexible routing: a file may send into a group, but not to the group it outputs to or to a plain track",
            sendOwn + " | " + sendToTrack);

    // The cap: 64 sends are fine; the 65th is refused (in a file, and no choice is offered)
    daw::Project many;
    many.tracks.push_back(trackOf(TrackKind::Audio, "Source"));
    for (size_t i = 0; i < audio::kMaxSends + 1; ++i)
        many.tracks.push_back(trackOf(i % 2 ? TrackKind::Group : TrackKind::Return, "Bus"));
    next = 1;
    many.normalizeIds(next);
    for (size_t i = 0; i < audio::kMaxSends; ++i)
        many.tracks[0].sends.push_back({many.tracks[i + 1].id, 0.1f, false});
    const std::string atCap = daw::routingError(many);
    const bool offered = daw::canSendTo(many, 0, 1) && !daw::canSendTo(many, 0, static_cast<int>(audio::kMaxSends) + 1);
    daw::Project over = many;
    over.tracks[0].sends.push_back({over.tracks[audio::kMaxSends + 1].id, 0.1f, false});
    daw::Project reread;
    std::string whyOver, whyAt;
    const bool readOver = daw::readProject(daw::writeProject(over), reread, whyOver);
    const bool readAt = daw::readProject(daw::writeProject(many), reread, whyAt);
    c.check(atCap.empty() && offered && !daw::routingError(over).empty() && !readOver && whyOver.find("at most 64") != std::string::npos &&
                readAt && reread.tracks[0].sends.size() == audio::kMaxSends,
            "Flexible routing: 64 sends are allowed, a 65th is refused in a file and not offered", atCap + " | " + whyOver + " | " + whyAt);
}

// The "Also to" menus: the choices, ticking as one edit and one undo step, the labels, deleting a group
void flexibleMenuChecks(Checker &c)
{
    daw::Project p;
    p.tracks = {trackOf(TrackKind::Audio, "A"), trackOf(TrackKind::Group, "Group 1"), trackOf(TrackKind::Group, "Group 2"),
                trackOf(TrackKind::Group, "A rather long group name"), trackOf(TrackKind::Return, "Return A"),
                trackOf(TrackKind::Group, "Drums")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId g1 = p.tracks[1].id, g2 = p.tracks[2].id, g3 = p.tracks[3].id, ret = p.tracks[4].id;
    p.tracks[0].output = g1;
    p.tracks[2].output = g1; // Group 2 goes into Group 1

    // Offered: the groups it may send to, never its output group, nor a group that feeds it, nor a return
    c.check(daw::alsoToChoices(p, 0) == std::vector<int>{2, 3, 5}, "Also to: every group but the output group is offered");
    c.check(daw::alsoToChoices(p, 1) == std::vector<int>{3, 5} && daw::alsoToChoices(p, 2) == std::vector<int>{3, 5},
            "Also to: a group is offered the groups it doesn't feed or go into (and not itself)");
    p.tracks[0].sends = {{g2, 0.5f, false}, {ret, 0.5f, false}};
    c.check(daw::alsoToChoices(p, 0) == std::vector<int>{2, 3, 5} && daw::groupSends(p, 0) == std::vector<int>{2},
            "Also to: a group already sent to stays offered (ticked); sends to returns aren't group sends");

    // Labels: 0, 1 and 2 extra destinations (returns not counted), long names cut
    c.check(daw::outputLabel(p, 0) == "To Group 1 + 1" && daw::outputName(p, 1) == "Master" && daw::outputLabel(p, 1) == "To Master" &&
                daw::extraDestinations(p, 0) == " + 1" && daw::extraDestinations(p, 1).empty(),
            "Also to: the output label counts the groups sent to, not returns", daw::outputLabel(p, 0));
    p.tracks[0].sends.push_back({g3, 1.0f, false});
    p.tracks[0].output = daw::kNoId;
    c.check(daw::outputLabel(p, 0) == "To Master + 2", "Also to: two extra destinations read \"+ 2\"", daw::outputLabel(p, 0));
    p.tracks[0].output = g1;
    p.tracks[1].output = g3;
    c.check(daw::outputLabel(p, 1).size() <= 17 && daw::outputLabel(p, 1).find("..") != std::string::npos,
            "Also to: a long group name is cut to fit the strip", daw::outputLabel(p, 1));
    c.check(daw::groupSendLabel("Group 1") == "G1" && daw::groupSendLabel("Group 12") == "G12" && daw::groupSendLabel("Drums") == "Dr" &&
                daw::groupSendLabel("Group x") == "Gr",
            "Also to: a group's send bar is labelled G1, or its first two letters");
    {
        daw::Project alike;
        alike.tracks = {trackOf(TrackKind::Group, "Drums"), trackOf(TrackKind::Group, "Group 1"), trackOf(TrackKind::Group, "Drum bus"),
                        trackOf(TrackKind::Audio, "Dry")};
        const std::string a = daw::groupSendLabel(alike, 0), b = daw::groupSendLabel(alike, 1), d = daw::groupSendLabel(alike, 2);
        c.check(a == "Dr1" && b == "G1" && d == "Dr2" && daw::groupSendLabel(alike, 3) == "Dr" && daw::groupSendLabel(alike, 9).empty(),
                "Also to: groups whose short names match are numbered, so no two bars read alike", a + ", " + b + ", " + d);
    }

    // Ticking and unticking are one edit each, with their undo steps; unticking takes the lane too
    p.tracks[0].sends.clear();
    daw::ProjectStore store(p);
    daw::History history;
    history.reset(store.project());
    double now = 1.0;
    daw::setAlsoTo(store, 0, g2, true);
    history.update(store, true, false, now);
    const std::string added = history.undoLabel();
    const bool has = daw::findSend(store.project().tracks[0], g2) && daw::findSend(store.project().tracks[0], g2)->level == 1.0f &&
                     !daw::findSend(store.project().tracks[0], g2)->preFader;
    daw::setAlsoTo(store, 0, g1, true); // its output group: refused, nothing changes
    const bool refusedOwn = store.project().tracks[0].sends.size() == 1;
    now += 2.0;
    store.editTrack(0).automation.push_back({daw::sendParam(g2), {{0.0, 0.5f}}});
    history.update(store, true, false, now);
    now += 2.0;
    daw::setAlsoTo(store, 0, g2, false);
    history.update(store, true, false, now);
    const std::string removed = history.undoLabel();
    const bool gone = store.project().tracks[0].sends.empty() && store.project().tracks[0].automation.empty();
    daw::HistoryRestore plan;
    const bool undone = history.undo(store, now + 2.0, plan);
    store.restore(std::move(plan));
    c.check(added == "Add send" && has && refusedOwn && removed == "Remove send" && gone && undone &&
                store.project().tracks[0].sends.size() == 1 &&
                audio::findAutomation(store.project().tracks[0].automation, daw::sendParam(g2)) != nullptr,
            "Also to: ticking adds a post-fader send at 0 dB, unticking takes it and its lane in one step that undo brings back",
            added + ", " + removed);

    // Deleting a group tracks send to takes the sends and lanes; one undo brings them back
    {
        daw::Project q = p;
        q.tracks[0].sends = {{g2, 1.0f, false}, {g3, 0.5f, false}};
        q.tracks[0].automation.push_back({daw::sendParam(g3), {{0.0, 0.5f}}});
        daw::ProjectStore s2(q);
        daw::History h2;
        h2.reset(s2.project());
        s2.edit().deleteTrack(3);
        h2.update(s2, true, false, 1.0);
        const daw::Track &a = s2.project().tracks[0];
        const bool taken = a.sends.size() == 1 && a.sends[0].to == g2 && audio::findAutomation(a.automation, daw::sendParam(g3)) == nullptr &&
                           daw::routingError(s2.project()).empty();
        daw::HistoryRestore restore;
        const bool ok = h2.undo(s2, 3.0, restore);
        s2.restore(std::move(restore));
        const daw::Track &b = s2.project().tracks[0];
        c.check(taken && ok && b.sends == q.tracks[0].sends && audio::findAutomation(b.automation, daw::sendParam(g3)) != nullptr,
                "Also to: deleting a group takes the sends into it and their lanes, and one undo brings them back");
    }
}

// The plan with sends into groups: a group fed by an output and a send is staged after both; delays; solo
void flexiblePlanChecks(Checker &c)
{
    daw::Project p;
    p.tracks = {trackOf(TrackKind::Audio, "A"), trackOf(TrackKind::Audio, "B"), trackOf(TrackKind::Group, "G")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId g = p.tracks[2].id;
    p.tracks[0].output = g;                 // A -> G -> master
    p.tracks[1].sends = {{g, 1.0f, false}}; // B -> master, and B -> G by a send
    // A's plugins delay it 100 frames: the group's input waits for it, so B's send to the group is held back 100 too
    const daw::RoutingPlan plan = daw::planRouting(p, {100, 0, 0}, 16384);
    const auto &n = plan.nodes;
    c.check(plan.stages == 2 && n[0].stage == 0 && n[1].stage == 0 && n[2].stage == 1 && n[1].sends.size() == 1 &&
                n[1].sends[0].to == 2 && n[0].output.to == 2,
            "Flexible plan: a group fed by an output and by a send is staged after both");
    c.check(n[0].output.delay == 0 && n[1].sends[0].delay == 100 && n[1].output.delay == 100 && plan.masterTime == 100,
            "Flexible plan: a send into a group lines up with the output that arrives late, and so does B's own output",
            printed("send %u, output %u, master %u", n[1].sends[0].delay, n[1].output.delay, plan.masterTime));

    // A diamond: T outputs to G1 and sends to G2, which outputs to G1; G2's 40 frames of latency hold T's output back
    daw::Project d;
    d.tracks = {trackOf(TrackKind::Audio, "T"), trackOf(TrackKind::Group, "G1"), trackOf(TrackKind::Group, "G2")};
    next = 1;
    d.normalizeIds(next);
    d.tracks[0].output = d.tracks[1].id;
    d.tracks[0].sends = {{d.tracks[2].id, 0.5f, false}};
    d.tracks[2].output = d.tracks[1].id;
    c.check(daw::routingError(d).empty(), "Flexible plan: the diamond is a sound routing", daw::routingError(d));
    const daw::RoutingPlan diamond = daw::planRouting(d, {0, 0, 40}, 16384);
    const auto &m = diamond.nodes;
    c.check(diamond.stages == 3 && m[0].stage == 0 && m[2].stage == 1 && m[1].stage == 2 && m[0].output.delay == 40 &&
                m[0].sends[0].delay == 0 && m[2].output.delay == 0 && diamond.masterTime == 40,
            "Flexible plan: a track feeding a group directly and through another group has its direct path held back",
            printed("output %u, send %u, master %u", m[0].output.delay, m[0].sends[0].delay, diamond.masterTime));

    // A send to the group a track outputs to is a doubled route: the plan keeps the output only
    daw::Project same = d;
    same.tracks[0].sends = {{same.tracks[1].id, 1.0f, false}};
    const daw::RoutingPlan dropped = daw::planRouting(same, {}, 16384);
    c.check(dropped.nodes[0].output.to == 1 && dropped.nodes[0].sends.size() == 1 && dropped.nodes[0].sends[0].to == -1,
            "Flexible plan: a send to the group the track outputs to is dropped, the output kept");

    // Solo spreads through a send into a group: a soloed sender plays the group; a soloed group plays what feeds it
    daw::Project q = p;
    q.tracks[1].solo = true;
    const daw::RoutingPlan s1 = daw::planRouting(q, {}, 16384);
    c.check(s1.nodes[1].soloed && s1.nodes[2].soloed && !s1.nodes[0].soloed, "Flexible plan: soloing a track keeps the group it sends to");
    q.tracks[1].solo = false;
    q.tracks[2].solo = true;
    const daw::RoutingPlan s2 = daw::planRouting(q, {}, 16384);
    c.check(s2.nodes[0].soloed && s2.nodes[1].soloed && s2.nodes[2].soloed,
            "Flexible plan: soloing a group keeps what sends to it and what goes into it");
}

// Through the bridge: a synth track feeding two groups, groups fed by an output and a send, and up to 64 sends
void flexibleEngineChecks(Checker &c, env::Environment &environment)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "vibin-routing-selftest").u8string();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    daw::Settings settings;
    settings.libraryPath = dir;
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    audio::MidiHub &midi = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
    daw::AudioBridge::SyncOptions options;
    options.listen = false;

    // 0 Source, 1 Group 1, 2 Group 2, then audio::kMaxSends returns
    daw::Project p;
    p.tracks = {sourceTrack("Source"), trackOf(TrackKind::Group, "Group 1"), trackOf(TrackKind::Group, "Group 2")};
    for (size_t i = 0; i < audio::kMaxSends; ++i)
        p.tracks.push_back(trackOf(TrackKind::Return, "Return"));
    daw::ProjectStore store(p);
    bridge.sync(store, options);
    const daw::ObjectId g1 = store.project().tracks[1].id, g2 = store.project().tracks[2].id;
    auto level = [&] {
        bridge.sync(store, options);
        return rmsOf(playFromStart(engine, 2.0), 24000, 48000);
    };
    const double alone = level();
    auto ratio = [&](const char *what, double wanted) {
        const double got = level() / std::max(alone, 1e-12);
        c.check(alone > 1e-3 && near(got, wanted, 0.005 * wanted + 1e-6), what,
                printed("%.4f of the track alone (wanted %.4f)", got, wanted));
    };
    const double k = kCentre;

    // The track outputs to Group 1 and sends half of itself to Group 2 (post-fader): the source reaches the master through
    // Group 1's pan, and through Group 2's with the send's half. Each path alone, then both.
    store.editMix(0).output = g1;
    store.editMix(0).sends = {{g2, 0.5f, false}};
    ratio("Flexible routing: a track outputting to Group 1 and sending half to Group 2 is heard through both", 1.5 * k);
    store.editMix(2).mute = true;
    ratio("Flexible routing: with Group 2 muted only the output through Group 1 is left", k);
    store.editMix(2).mute = false;
    store.editMix(1).mute = true;
    ratio("Flexible routing: with Group 1 muted only the send through Group 2 is left", 0.5 * k);
    store.editMix(1).mute = false;
    store.editMix(0).sends[0].preFader = true;
    store.editMix(0).gain = 0.5f;
    ratio("Flexible routing: a pre-fader send into a group ignores the fader (the output is at half)", 0.5 + 0.5 * k);
    store.editMix(0).sends[0].preFader = false;
    store.editMix(0).gain = 1.0f;

    // A group fed by an output (Group 1 into Group 2) and by a send (the track's): both arrive and are summed before its pan
    store.editMix(1).output = g2;
    ratio("Flexible routing: a group fed by another group's output and by a send plays the sum", k * k + 0.5 * k);
    store.editMix(1).output = daw::kNoId;
    store.editMix(0).output = daw::kNoId;
    store.editMix(0).sends.clear();

    // More than 16 sends: 20 returns at 0.1 are all heard, the last four alone with the first 16 muted, and all 64 at the cap
    auto sendTo = [&](size_t count, float each) {
        std::vector<daw::Send> sends;
        for (size_t i = 0; i < count; ++i)
            sends.push_back({store.project().tracks[3 + i].id, each, false});
        store.editMix(0).sends = sends;
    };
    sendTo(20, 0.1f);
    ratio("Flexible routing: a track with 20 sends is heard through all 20 returns", 1.0 + 20 * 0.1 * k);
    for (int i = 0; i < 16; ++i)
        store.editMix(3 + i).mute = true;
    ratio("Flexible routing: the 17th to 20th sends play with the first 16 returns muted", 1.0 + 4 * 0.1 * k);
    for (int i = 0; i < 16; ++i)
        store.editMix(3 + i).mute = false;
    sendTo(audio::kMaxSends, 0.02f);
    ratio("Flexible routing: all 64 sends are heard", 1.0 + 64 * 0.02 * k);
    bridge.dropAll();
}

// Master automation: the file, undo and the engine playing the master volume's lane
void masterAutomationChecks(Checker &c, env::Environment &environment)
{
    const float unity = static_cast<float>(std::cbrt(0.5)); // a lane position along the fader's taper (max 2) that is gain 1
    daw::Project p;
    p.tracks = {sourceTrack("Source"), trackOf(TrackKind::Audio, "Audio")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);

    // The model: the value at a beat is the lane's with points, else the master volume
    p.masterVolume = 0.5f;
    c.check(near(daw::masterVolumeAt(p, 3.0), 0.5), "Master automation: with no lane the master volume plays");
    p.masterAutomation.push_back({audio::kVolumeParam, {{0.0, 0.0f}, {4.0, 0.0f}, {8.0, unity}}});
    c.check(near(daw::masterVolumeAt(p, 2.0), 0.0) && near(daw::masterVolumeAt(p, 8.0), 1.0, 1e-3) &&
                near(daw::masterVolumeAt(p, 100.0), 1.0, 1e-3) && daw::masterVolumeAt(p, 6.0) > 0.0 &&
                daw::masterVolumeAt(p, 6.0) < 1.0,
            "Master automation: a lane's value follows its points and the fader's taper");

    // The file: master_automation at the root, read back exactly and written again the same
    const std::string text = daw::writeProject(p);
    daw::Project back;
    std::string why;
    const bool read = daw::readProject(text, back, why);
    c.check(read && text.find("master_automation = [") != std::string::npos && back.masterAutomation == p.masterAutomation &&
                daw::sameContent(p, back) && daw::sameSaved(p, back) && daw::writeProject(back) == text,
            "Master automation: the file round-trips the lane", why);
    daw::Project none = p;
    none.masterAutomation.clear();
    c.check(daw::writeProject(none).find("master_automation") == std::string::npos && !daw::sameContent(p, none) &&
                !daw::sameSaved(p, none),
            "Master automation: no key without lanes, and a lane is content and saved");
    {
        // Values clamped, points sorted, a second lane for volume dropped
        daw::Project q;
        const bool ok = daw::readProject("format = 3\nmaster_automation = [ { param = \"volume\", points = [ { beat = 4, value = 3 },"
                                         " { beat = 1, value = -1 } ] }, { param = \"volume\", points = [] } ]\n",
                                         q, why);
        c.check(ok && q.masterAutomation.size() == 1 && q.masterAutomation[0].points.size() == 2 &&
                    q.masterAutomation[0].points[0].beat == 1.0 && q.masterAutomation[0].points[0].value == 0.0f &&
                    q.masterAutomation[0].points[1].value == 1.0f,
                "Master automation: values are clamped and points sorted when read", why);
    }
    auto refused = [](const std::string &file) {
        daw::Project q;
        std::string reason;
        return daw::readProject(file, q, reason) ? std::string("(read)") : reason;
    };
    const std::string pan = refused("format = 3\nmaster_automation = [ { param = \"pan\", points = [] } ]\n");
    const std::string send = refused("format = 3\nmaster_automation = [ { param = \"send\", to = 2, points = [] } ]\n");
    const std::string id = refused("format = 3\nmaster_automation = [ { param = 3, points = [] } ]\n");
    c.check(pan.find("only lane is \"volume\"") != std::string::npos && send.find("only lane is \"volume\"") != std::string::npos &&
                id.find("volume") != std::string::npos,
            "Master automation: a pan, send or id lane on the master is refused with a reason", pan);

    // Undo: an edit is "Edit automation", and undoing it moves the master's revisions and no track's
    {
        daw::Project q = p;
        q.masterAutomation.clear();
        q.masterVolume = 1.0f;
        daw::ProjectStore store(q);
        daw::History history;
        history.reset(store.project());
        store.editMasterAutomation().push_back({audio::kVolumeParam, {{0.0, 0.25f}, {4.0, 0.5f}}});
        history.update(store, true, false, 1.0);
        c.check(history.undoLabel() == "Edit automation", "Master automation: the edit is an undo step named Edit automation",
                history.undoLabel());
        store.editMasterAutomation()[0].points[1].value = 0.75f;
        history.update(store, true, false, 3.0);
        c.check(history.undoLabel() == "Edit automation", "Master automation: changing a point is a step too", history.undoLabel());
        const uint64_t master = store.masterRevision(), t0 = store.trackRevision(0), t1 = store.trackRevision(1),
                       m0 = store.mixRevision(0), m1 = store.mixRevision(1), structure = store.structureRevision();
        daw::HistoryRestore plan;
        bool undone = history.undo(store, 5.0, plan);
        store.restore(std::move(plan));
        c.check(undone && store.project().masterAutomation.size() == 1 &&
                    store.project().masterAutomation[0].points[1].value == 0.5f && store.masterRevision() != master &&
                    store.trackRevision(0) == t0 && store.trackRevision(1) == t1 && store.mixRevision(0) == m0 &&
                    store.mixRevision(1) == m1 && store.structureRevision() == structure,
                "Master automation: undo puts the lane back through the master alone");
        undone = history.undo(store, 5.0, plan);
        store.restore(std::move(plan));
        c.check(undone && store.project().masterAutomation.empty() && store.trackRevision(0) == t0 && store.trackRevision(1) == t1,
                "Master automation: undoing the first edit leaves no lane");
        const uint64_t before = store.masterRevision();
        undone = history.redo(store, plan);
        store.restore(std::move(plan));
        c.check(undone && store.project().masterAutomation.size() == 1 && store.masterRevision() != before,
                "Master automation: redo puts the lane back");
    }

    // The engine, through the bridge: a lane silent until beat 4, then at unity, against no lane
    const std::string dir = (std::filesystem::temp_directory_path() / "vibin-routing-selftest").u8string();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    daw::Settings settings;
    settings.libraryPath = dir;
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    audio::MidiHub &midi = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
    daw::AudioBridge::SyncOptions options;
    options.listen = false;
    daw::Project e;
    e.tracks = {sourceTrack("Source")};
    daw::ProjectStore store(e);
    bridge.sync(store, options);
    const std::vector<float> plain = playFromStart(engine, 8.0);
    const double plainLate = rmsOf(plain, 5 * 24000, 8 * 24000);
    // (the lane rises over a hundredth of a beat at beat 4; the windows keep clear of it)
    store.editMasterAutomation().push_back({audio::kVolumeParam, {{0.0, 0.0f}, {4.0, 0.0f}, {4.01, unity}}});
    bridge.sync(store, options);
    const std::vector<float> automated = playFromStart(engine, 8.0);
    const double early = rmsOf(automated, 1 * 24000, 3 * 24000), late = rmsOf(automated, 5 * 24000, 8 * 24000);
    c.check(plainLate > 1e-3 && early < 1e-6 && near(late / std::max(plainLate, 1e-12), 1.0, 0.01),
            "Master automation: a lane silent until beat 4 renders silent, then as loud as without it",
            printed("early %.6f, late %.4f of the plain %.4f", early, late / std::max(plainLate, 1e-12), plainLate));
    // Position 0.5 on the taper is gain 2 x 0.5^3 = 0.25
    store.editMasterAutomation()[0].points = {{0.0, 0.5f}};
    bridge.sync(store, options);
    const double quarter = rmsOf(playFromStart(engine, 8.0), 5 * 24000, 8 * 24000) / std::max(plainLate, 1e-12);
    c.check(near(quarter, 0.25, 0.005), "Master automation: a lane's value is the master volume on the fader's taper (0.5 is gain 0.25)",
            printed("%.4f", quarter));
    // Without points the project's master volume plays again
    store.editMasterAutomation()[0].points.clear();
    bridge.sync(store, options);
    const double again = rmsOf(playFromStart(engine, 8.0), 5 * 24000, 8 * 24000) / std::max(plainLate, 1e-12);
    c.check(near(again, 1.0, 0.01), "Master automation: a lane with no points plays nothing", printed("%.4f", again));
    bridge.dropAll();
}

// 24 tracks with 4 returns and 2 groups against 24 plain tracks, through the bridge (a benchmark)
// Sidechains: a Compressor (or Gate) keyed by another track's sound. The model, the file, the plan
// and the engine
audio::DeviceSetting compressorKeyedBy(daw::ObjectId source)
{
    audio::DeviceSetting d = audio::effectSetting(audio::EffectType::Compressor);
    d.sidechain = source;
    return d;
}

// A "kick": a synth track with a short note on each of the first four beats
daw::Track kickTrack(const char *name)
{
    daw::Track t = trackOf(TrackKind::Instrument, name);
    t.instrument = audio::SynthDevice::setting();
    std::vector<daw::Note> notes;
    for (int beat = 0; beat < 4; ++beat)
        notes.push_back(daw::Note{static_cast<double>(beat), 0.25, 36, 0.9f});
    t.clips.push_back(daw::Clip("Kick", 0.0, 16.0, notes));
    return t;
}

void sidechainModelChecks(Checker &c)
{
    daw::Project p;
    p.tracks = {sourceTrack("Kick"), sourceTrack("Pad"), trackOf(TrackKind::Group, "G"), trackOf(TrackKind::Audio, "Audio"),
                trackOf(TrackKind::Return, "R")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId kick = p.tracks[0].id, audioId = p.tracks[3].id;

    // The file: sidechain = <id> in the device's table, read back; none written when unset
    p.tracks[1].effects = {compressorKeyedBy(kick)};
    p.masterEffects = {compressorKeyedBy(audioId)};
    const std::string text = daw::writeProject(p);
    daw::Project back;
    std::string why;
    const bool read = daw::readProject(text, back, why);
    c.check(read && text.find("sidechain = " + std::to_string(kick)) != std::string::npos &&
                text.find("sidechain = " + std::to_string(audioId)) != std::string::npos &&
                back.tracks[1].effects[0].sidechain == kick && back.masterEffects[0].sidechain == audioId &&
                back.tracks[1].effects[0] == p.tracks[1].effects[0] && daw::sameSaved(p, back) &&
                daw::sameContent(p, back) && daw::writeProject(back) == text,
            "Sidechain: the file round-trips a track's and the master's keyed effects", why);
    daw::Project plain = p;
    plain.tracks[1].effects[0].sidechain = daw::kNoId;
    plain.masterEffects[0].sidechain = daw::kNoId;
    c.check(daw::writeProject(plain).find("sidechain") == std::string::npos && !daw::sameSaved(p, plain) &&
                !daw::sameContent(p, plain) && plain.tracks[1].effects[0] != p.tracks[1].effects[0],
            "Sidechain: no key without one, and a sidechain is content, saved and part of a device's equality");

    // Refusals
    auto refused = [](const daw::Project &q) {
        daw::Project r;
        std::string reason;
        return daw::readProject(daw::writeProject(q), r, reason) ? std::string("(read)") : reason;
    };
    daw::Project unknown = p;
    unknown.tracks[1].effects[0].sidechain = 999;
    daw::Project eq = p;
    eq.tracks[1].effects = {audio::effectSetting(audio::EffectType::Equalizer)};
    eq.tracks[1].effects[0].sidechain = kick;
    daw::Project self = p;
    self.tracks[1].effects[0].sidechain = self.tracks[1].id;
    daw::Project masterUnknown = p;
    masterUnknown.masterEffects[0].sidechain = 999;
    daw::Project masterGain = p;
    masterGain.masterEffects = {audio::effectSetting(audio::EffectType::Gain)};
    masterGain.masterEffects[0].sidechain = kick;
    const std::string a = refused(unknown), b = refused(eq), s = refused(self), m = refused(masterUnknown), e = refused(masterGain);
    c.check(a.find("no track has") != std::string::npos && b.find("can't take a sidechain") != std::string::npos &&
                s.find("the track itself") != std::string::npos && m.find("no track has") != std::string::npos &&
                e.find("can't take a sidechain") != std::string::npos,
            "Sidechain: files with an unknown id, a key on an EQ or Gain, or a key from the track itself are refused",
            a + " | " + b + " | " + s);
    {
        daw::Project q;
        const bool ok = daw::readProject("format = 3\n[[tracks]]\nid = 1\nname = \"A\"\n[[tracks.effects]]\nkind = \"effect\"\n"
                                         "id = \"compressor\"\nsidechain = -2\n",
                                         q, why);
        c.check(!ok && why.find("sidechain") != std::string::npos, "Sidechain: a sidechain that isn't a track's id is refused", why);
    }
    // A key loop in a file (two tracks keyed by each other) is refused like any loop
    daw::Project loop = p;
    loop.masterEffects.clear();
    loop.tracks[0].effects = {compressorKeyedBy(p.tracks[1].id)};
    loop.tracks[1].effects = {compressorKeyedBy(kick)};
    c.check(!daw::routingError(loop).empty() && refused(loop).find("comes back") != std::string::npos,
            "Sidechain: two tracks keyed by each other are refused", refused(loop));

    // At most four keyed effects on a track, and on the master: more are refused (the engine has four key buffers)
    daw::Project four = p, five = p;
    four.masterEffects.assign(4, compressorKeyedBy(kick));
    four.tracks[1].effects.assign(4, compressorKeyedBy(kick));
    five.masterEffects.assign(5, compressorKeyedBy(kick));
    daw::Project fiveOnTrack = p;
    fiveOnTrack.tracks[1].effects.assign(5, compressorKeyedBy(kick));
    const std::string tooMany = refused(fiveOnTrack), tooManyMaster = refused(five);
    c.check(refused(four) == "(read)" && tooMany.find("at most 4") != std::string::npos &&
                tooManyMaster.find("at most 4") != std::string::npos,
            "Sidechain: four keyed effects are read, a fifth on a track or on the master is refused", tooMany + " | " + tooManyMaster);
    {
        // The offer: the fifth effect gets no choices and the reason, a keyed one keeps its own
        daw::Project q = p;
        q.masterEffects.clear();
        q.tracks[1].effects.assign(4, compressorKeyedBy(kick));
        q.tracks[1].effects.push_back(audio::effectSetting(audio::EffectType::Gate));
        q.tracks[1].effects.push_back(audio::effectSetting(audio::EffectType::Equalizer));
        const daw::SidechainOffer fifth = daw::sidechainOffer(q, daw::DevicePlace::effect(false, 1, 4));
        const daw::SidechainOffer keyedOne = daw::sidechainOffer(q, daw::DevicePlace::effect(false, 1, 0));
        const daw::SidechainOffer eqOffer = daw::sidechainOffer(q, daw::DevicePlace::effect(false, 1, 5));
        c.check(fifth.available && fifth.sources.empty() && fifth.note == "4 sidechains per track at most" &&
                    keyedOne.available && keyedOne.current == kick && !keyedOne.sources.empty() && !eqOffer.available,
                "Sidechain: the offer for a fifth keyed effect is empty with the reason; a keyed one and an EQ are as they should be");
    }

    // canSidechain: another track of any kind, never itself, never one the track reaches
    p.tracks[1].output = p.tracks[2].id; // Pad into G
    c.check(daw::canSidechain(p, 1, 0) && daw::canSidechain(p, 1, 3) && daw::canSidechain(p, 1, 4) && !daw::canSidechain(p, 1, 1) &&
                !daw::canSidechain(p, 1, 2) && daw::canSidechain(p, 2, 1) && !daw::canSidechain(p, 1, 9) &&
                !daw::canSidechain(p, -1, 0),
            "Sidechain: any other track may key a track's effect, but not itself or a track it feeds");
    // The pad is keyed by the kick, so a key is a route: the kick can't be keyed by the pad (a loop)
    c.check(!daw::canSidechain(p, 0, 1) && daw::canSidechain(p, 0, 3),
            "Sidechain: a key counts as a route, so a loop through keys is refused");

    // forgetTrack: deleting the source clears the sidechains to it, on tracks' devices and the master's
    daw::Project q = p;
    q.tracks[2].effects = {audio::effectSetting(audio::EffectType::Gate)};
    q.tracks[2].effects[0].sidechain = kick;
    q.masterEffects.push_back(compressorKeyedBy(kick));
    q.deleteTrack(0);
    bool cleared = q.masterEffects[0].sidechain == audioId && q.masterEffects[1].sidechain == daw::kNoId;
    for (const daw::Track &t : q.tracks) {
        for (const audio::DeviceSetting &d : t.effects)
            cleared = cleared && d.sidechain != kick;
    }
    c.check(cleared && q.tracks[1].effects[0].sidechain == daw::kNoId && daw::routingError(q).empty(),
            "Sidechain: deleting the source track clears the sidechains to it, tracks' and the master's");
    q.deleteTrack(2); // the audio track the master's first compressor listens to
    c.check(q.masterEffects[0].sidechain == daw::kNoId && daw::routingError(q).empty(),
            "Sidechain: a deleted audio track no longer keys the master");

    // Undo's name
    daw::ProjectStore store(p);
    daw::History history;
    history.reset(store.project());
    store.editTrack(1).effects[0].sidechain = daw::kNoId;
    history.update(store, true, false, 1.0);
    c.check(history.undoLabel() == "Sidechain", "Sidechain: changing the key is an undo step named \"Sidechain\"", history.undoLabel());
}

void sidechainPlanChecks(Checker &c)
{
    // The keyed track comes first, so the source must still be staged before it, and a group that is
    // fed late takes the key held back to line up
    daw::Project p;
    p.tracks = {trackOf(TrackKind::Group, "G"), trackOf(TrackKind::Instrument, "Slow"), trackOf(TrackKind::Instrument, "Kick")};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    p.tracks[1].output = p.tracks[0].id;
    p.tracks[0].effects = {audio::effectSetting(audio::EffectType::Gain), compressorKeyedBy(p.tracks[2].id)};
    daw::RoutingPlan plan = daw::planRouting(p, {0, 300, 0}, 16384);
    c.check(plan.nodes[2].stage < plan.nodes[0].stage && plan.nodes[0].keys.size() == 1 && plan.nodes[0].keys[0].from == 2 &&
                plan.nodes[0].keys[0].effect == 1 && plan.nodes[0].keys[0].delay == 300 && plan.nodes[1].stage < plan.nodes[0].stage,
            "Sidechain plan: the source is staged before the keyed track and its key is held to the track's input",
            printed("stages %d, %d, %d; %zu key(s), delay %u", plan.nodes[0].stage, plan.nodes[1].stage, plan.nodes[2].stage,
                    plan.nodes[0].keys.size(), plan.nodes[0].keys.empty() ? 0u : plan.nodes[0].keys[0].delay));
    // A key carries no sound: the kick still goes to the master, and soloing the keyed track doesn't solo the kick
    p.tracks[0].solo = true;
    plan = daw::planRouting(p, {0, 0, 0}, 16384);
    c.check(plan.nodes[2].output.to == -1 && plan.nodes[0].soloed && plan.nodes[1].soloed && !plan.nodes[2].soloed,
            "Sidechain plan: a key adds no sound, so solo doesn't reach the source through it");

    // Keys that would loop are dropped, in track order (like sends)
    daw::Project loop;
    loop.tracks = {trackOf(TrackKind::Instrument, "A"), trackOf(TrackKind::Instrument, "B")};
    loop.normalizeIds(next);
    loop.tracks[0].effects = {compressorKeyedBy(loop.tracks[1].id)};
    loop.tracks[1].effects = {compressorKeyedBy(loop.tracks[0].id)};
    plan = daw::planRouting(loop, {0, 0}, 16384);
    c.check(plan.nodes[0].keys.size() == 1 && plan.nodes[1].keys.empty() && plan.nodes[1].stage < plan.nodes[0].stage,
            "Sidechain plan: a key that would loop is dropped",
            printed("%zu and %zu key(s)", plan.nodes[0].keys.size(), plan.nodes[1].keys.size()));
    // A key from a track the keyed track feeds is a loop too
    daw::Project fed;
    fed.tracks = {trackOf(TrackKind::Instrument, "Src"), trackOf(TrackKind::Group, "G")};
    fed.normalizeIds(next);
    fed.tracks[0].output = fed.tracks[1].id;
    fed.tracks[1].effects = {compressorKeyedBy(fed.tracks[0].id)}; // fine: the source feeds the group
    fed.tracks[0].effects = {compressorKeyedBy(fed.tracks[1].id)}; // a loop
    plan = daw::planRouting(fed, {0, 0}, 16384);
    c.check(plan.nodes[0].keys.empty() && plan.nodes[1].keys.size() == 1 && plan.nodes[0].stage < plan.nodes[1].stage,
            "Sidechain plan: a key from the track's own destination is dropped, the group's key from the track stays");
    // The master's keys
    daw::Project m;
    m.tracks = {trackOf(TrackKind::Instrument, "Kick")};
    m.normalizeIds(next);
    m.masterEffects = {compressorKeyedBy(m.tracks[0].id)};
    plan = daw::planRouting(m, {200}, 16384);
    c.check(plan.masterKeys.size() == 1 && plan.masterKeys[0].from == 0 && plan.masterKeys[0].effect == 0,
            "Sidechain plan: the master's effects may be keyed by any track");
}

// The level (dB) of the mix while a kick note sounds, and between the notes
struct Dips {
    double during = 0.0, between = 0.0;
    double difference() const { return between - during; }
};

Dips measureDips(audio::AudioSystem &engine)
{
    const std::vector<float> out = playFromStart(engine, 4.0);
    double during = 0.0, between = 0.0;
    int n = 0;
    for (size_t beat = 1; beat < 4; ++beat, ++n) {
        const size_t b = beat * 24000;
        during += rmsOf(out, b + 1500, b + 4500);
        between += rmsOf(out, b + 18000, b + 23000);
    }
    Dips d;
    d.during = 20.0 * std::log10(std::max(during / n, 1e-9));
    d.between = 20.0 * std::log10(std::max(between / n, 1e-9));
    return d;
}

void sidechainEngineChecks(Checker &c, env::Environment &environment)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "vibin-sidechain-selftest").u8string();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    daw::Settings settings;
    settings.libraryPath = dir;
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    audio::MidiHub &midi = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
    daw::AudioBridge::SyncOptions options;
    options.listen = false;

    // A held pad with a fast, hard compressor whose threshold only a loud kick passes, and a kick whose own
    // output is muted with its fader down (the key is its sound after its effects, before both)
    daw::Project p;
    p.tracks = {sourceTrack("Pad"), kickTrack("Kick")};
    p.tracks[1].mute = true;
    p.tracks[1].gain = 0.0f;
    audio::DeviceSetting boost = audio::effectSetting(audio::EffectType::Gain);
    boost.setValue(0, 24.0); // +24 dB
    p.tracks[1].effects = {boost};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    audio::DeviceSetting comp = audio::effectSetting(audio::EffectType::Compressor);
    comp.setValue(0, -12.0); // threshold: over the kick (about -1 dB after its boost), under nothing the pad plays
    comp.setValue(1, 20.0);  // ratio
    comp.setValue(2, 0.1);   // attack
    comp.setValue(3, 10.0);  // release
    comp.setValue(4, 0.0);   // knee
    p.tracks[0].effects = {comp};
    const daw::ObjectId kickId = p.tracks[1].id;
    daw::ProjectStore store(p);
    bridge.sync(store, options);
    auto dips = [&] {
        bridge.sync(store, options);
        return measureDips(engine);
    };

    const Dips unkeyed = dips();
    store.editTrack(0).effects[0].sidechain = kickId;
    const Dips keyed = dips();
    c.check(std::fabs(unkeyed.difference()) < 1.0, "Sidechain engine: with no key the pad's level is steady through the kick's beats",
            printed("%.1f dB during, %.1f dB between", unkeyed.during, unkeyed.between));
    c.check(keyed.difference() > 6.0 && std::fabs(keyed.between - unkeyed.between) < 1.0,
            "Sidechain engine: a compressor keyed by a muted kick ducks the pad on each kick and not between",
            printed("%.1f dB during, %.1f dB between: %.1f dB of ducking", keyed.during, keyed.between, keyed.difference()));

    store.editTrack(0).effects[0].sidechain = daw::kNoId;
    const Dips released = dips();
    c.check(std::fabs(released.difference()) < 1.0,
            "Sidechain engine: removing the sidechain brings the steady level back (no key left in the chain)",
            printed("%.1f dB during, %.1f dB between", released.during, released.between));

    // The key reaches a master effect too: the same compressor on the master, the pad's own compressor off
    store.editTrack(0).effects.clear();
    store.editMasterEffects() = {comp};
    store.editMasterEffects()[0].sidechain = kickId;
    const Dips master = dips();
    c.check(master.difference() > 6.0, "Sidechain engine: the master's compressor can be keyed by a track too",
            printed("%.1f dB during, %.1f dB between", master.during, master.between));
    store.editMasterEffects()[0].sidechain = daw::kNoId;
    const Dips masterPlain = dips();
    c.check(std::fabs(masterPlain.difference()) < 1.0, "Sidechain engine: and dropping the master's key ends the ducking",
            printed("%.1f dB during, %.1f dB between", masterPlain.during, masterPlain.between));
    store.editMasterEffects().clear();
}

// A constant 0.5 on both channels (a steady sound for a keyed effect to act on)
class Steady : public audio::AudioProcessor {
public:
    Steady() : AudioProcessor("Steady") {}
    void process(audio::AudioBlock &block) override
    {
        for (uint32_t c = 0; c < block.channelCount; ++c)
            for (uint32_t i = 0; i < block.frames; ++i)
                block.channels[c][i] += 0.5f;
    }
};

// A hand-made routing: an impulse track keys a compressor on a steady track through a key held back
// `delay` frames. The steady track's output starts to fall where the key arrives: the impulse's frame
// plus the delay.
int firstDropWithKeyDelay(env::Environment &environment, uint32_t delay)
{
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    auto group = std::make_shared<audio::SoloGroup>();
    constexpr int kAt = 60; // blocks of 512 before the impulse: every level has glided home
    audio::TrackProcessor &source = engine.add<audio::TrackProcessor>("Key", group, std::make_unique<Impulse>(kAt));
    audio::TrackProcessor &keyed = engine.add<audio::TrackProcessor>("Keyed", group, std::make_unique<Steady>());
    source.mute = true; // the key is heard by the compressor alone
    audio::DeviceSetting comp = audio::effectSetting(audio::EffectType::Compressor);
    comp.setValue(0, -60.0); // threshold
    comp.setValue(1, 20.0);  // ratio
    comp.setValue(2, 0.1);   // attack
    comp.setValue(3, 100.0); // release
    comp.setValue(4, 0.0);   // knee
    keyed.chain.apply({comp});
    auto routing = std::make_unique<audio::Routing>();
    routing->beginStage();
    routing->add({&source, nullptr, {}, {}, {}});
    routing->beginStage();
    routing->add({&keyed, nullptr, {}, {}, {{&source, 0, delay}}});
    engine.setRouting(std::move(routing));
    std::vector<float> out(512 * 2);
    for (int b = 0; b < kAt; ++b)
        engine.renderOffline(out.data(), 512, 48000.0);
    const float steady = out[0];
    engine.renderOffline(out.data(), 512, 48000.0); // the impulse's block
    for (int i = 0; i < 512; ++i) {
        if (out[static_cast<size_t>(i) * 2] < steady * 0.99f)
            return i;
    }
    return -1;
}

void sidechainDelayChecks(Checker &c, env::Environment &environment)
{
    const int none = firstDropWithKeyDelay(environment, 0), a = firstDropWithKeyDelay(environment, 100),
              b = firstDropWithKeyDelay(environment, 250);
    c.check(none == 0 && a == 100 && b == 250,
            "Sidechain engine: a key held back by its delay reaches the effect that many frames later (delay compensation)",
            printed("first fall at frame %d, %d, %d for delays 0, 100, 250", none, a, b));
}

// The gate's level while a kick sounds and between the notes, keyed and not
void sidechainGateChecks(Checker &c, env::Environment &environment)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "vibin-sidechain-gate").u8string();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    daw::Settings settings;
    settings.libraryPath = dir;
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine.masterGain().set(1.0f);
    audio::MidiHub &midi = engine.add<audio::MidiHub>();
    daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
    daw::AudioBridge::SyncOptions options;
    options.listen = false;

    daw::Project p;
    p.tracks = {sourceTrack("Pad"), kickTrack("Kick")};
    p.tracks[1].mute = true;
    p.tracks[1].gain = 0.0f;
    audio::DeviceSetting boost = audio::effectSetting(audio::EffectType::Gain);
    boost.setValue(0, 24.0);
    p.tracks[1].effects = {boost};
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    audio::DeviceSetting gate = audio::effectSetting(audio::EffectType::Gate);
    gate.setValue(0, -12.0); // threshold: over the boosted kick, above the pad's own level
    gate.setValue(1, 0.5);   // attack
    gate.setValue(2, 5.0);   // release
    gate.setValue(3, -40.0); // range
    p.tracks[0].effects = {gate};
    const daw::ObjectId kickId = p.tracks[1].id;
    daw::ProjectStore store(p);
    auto dips = [&] {
        bridge.sync(store, options);
        return measureDips(engine);
    };
    const Dips unkeyed = dips();
    store.editTrack(0).effects[0].sidechain = kickId;
    const Dips keyed = dips();
    c.check(std::fabs(unkeyed.difference()) < 1.0 && unkeyed.during < -50.0,
            "Sidechain engine: an unkeyed gate sees the pad's own level, under its threshold, and stays closed",
            printed("%.1f dB during, %.1f dB between", unkeyed.during, unkeyed.between));
    c.check(keyed.difference() < -15.0 && std::fabs(keyed.during - (-24.8)) < 2.0 && keyed.between < -50.0,
            "Sidechain engine: a gate keyed by the kick opens on each kick and closes to its range between",
            printed("%.1f dB during, %.1f dB between", keyed.during, keyed.between));
}

void benchChecks(Checker &c, env::Environment &environment)
{
    if (!bench()) {
        c.skip("Routing bench", "set VIBIN_TEST_BENCH to time it");
        return;
    }
    // One return bus taking in 24 sends (half before the fader), on this thread alone: what the sends
    // themselves cost, apart from the pool and the instruments
    {
        auto group = std::make_shared<audio::SoloGroup>();
        std::vector<std::unique_ptr<audio::TrackProcessor>> sources;
        audio::TrackProcessor ret("R", group);
        ret.prepare(48000.0, 512);
        auto routing = std::make_unique<audio::Routing>();
        routing->beginStage();
        for (int i = 0; i < 24; ++i) {
            sources.push_back(std::make_unique<audio::TrackProcessor>("S", group, std::make_unique<Impulse>(-1)));
            sources.back()->prepare(48000.0, 512);
            sources.back()->setSendLevel(0, 0.3f);
            routing->add({sources.back().get(), nullptr, {{&ret, 0, 0, 0, i % 2 == 1}}, {}, {}});
        }
        routing->beginStage();
        routing->add({&ret, nullptr, {}, {}, {}});
        routing->finish();
        const audio::Route &bus = routing->routes.back();
        std::vector<float> l(512), r(512);
        float *channels[2] = {l.data(), r.data()};
        audio::TransportState state;
        const audio::AudioBlock block{channels, 2, 512, 48000.0, state};
        for (auto &s : sources)
            s->renderRouted(block, routing->routes[0]);
        for (int i = 0; i < 100; ++i) // levels settle
            ret.renderRouted(block, bus);
        constexpr int kBlocks = 2000;
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < kBlocks; ++i)
            ret.renderRouted(block, bus);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / kBlocks;
        const audio::Route empty{&ret, nullptr, {}, {}, {}};
        const auto bare = std::chrono::steady_clock::now();
        for (int i = 0; i < kBlocks; ++i)
            ret.renderRouted(block, empty);
        const double bareUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - bare).count() / kBlocks;
        std::printf("INFO routing bus: a return taking in 24 sends renders a 512-frame block in %.1f us on one thread "
                    "(%.1f us with nothing coming in)\n",
                    us, bareUs);
    }

    daw::Settings settings;
    settings.libraryPath = (std::filesystem::temp_directory_path() / "vibin-routing-selftest").u8string();
    daw::InstrumentLibrary library(environment, settings);
    daw::AudioPool pool(environment);
    auto time = [&](int mode) {
        const bool routed = mode > 0;
        audio::AudioSystem engine(environment, audio::Deviceless{});
        engine.setPool(&environment.find<env::Scheduler>()->realtime());
        audio::MidiHub &midi = engine.add<audio::MidiHub>();
        daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
        daw::AudioBridge::SyncOptions options;
        options.listen = false;
        daw::Project p;
        for (int i = 0; i < 24; ++i)
            p.tracks.push_back(sourceTrack("S"));
        if (routed) {
            for (int i = 0; i < 4; ++i)
                p.tracks.push_back(trackOf(TrackKind::Return, "R"));
            for (int i = 0; i < 2; ++i)
                p.tracks.push_back(trackOf(TrackKind::Group, "G"));
        }
        daw::ObjectId next = 1;
        p.normalizeIds(next);
        if (routed) {
            for (int i = 0; i < 24; ++i) {
                for (int r = 0; r < 4; ++r)
                    p.tracks[static_cast<size_t>(i)].sends.push_back({p.tracks[static_cast<size_t>(24 + r)].id, 0.3f, r % 2 == 1});
                p.tracks[static_cast<size_t>(i)].output = p.tracks[static_cast<size_t>(28 + i % 2)].id;
                if (mode == 2) // and a send into the other group
                    p.tracks[static_cast<size_t>(i)].sends.push_back({p.tracks[static_cast<size_t>(28 + (i + 1) % 2)].id, 0.3f, false});
            }
        }
        daw::ProjectStore store(p);
        bridge.sync(store, options);
        playFromStart(engine, 1.0); // warm
        const auto begin = std::chrono::steady_clock::now();
        playFromStart(engine, 8.0);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    };
    // Each the fastest of three runs, interleaved, so a busy moment on the machine doesn't land on one alone
    double plain = 1e30, routed = 1e30, toGroups = 1e30;
    for (int run = 0; run < 3; ++run) {
        plain = std::min(plain, time(0));
        routed = std::min(routed, time(1));
        toGroups = std::min(toGroups, time(2));
    }
    std::printf("INFO routing bench: 4 s of 24 synth tracks: plain %.1f ms, with 4 returns (96 sends) and 2 groups %.1f ms (%+.0f%%), each also sending into the other group "
                "(120 sends) %.1f ms (%+.0f%%)\n",
                plain, routed, (routed / plain - 1.0) * 100.0, toGroups, (toGroups / plain - 1.0) * 100.0);
    // Sidechains: the same 24 synth tracks and a kick track, with no effects, a compressor each, and a
    // compressor each keyed by the kick (a key copied from the kick's delay line into every track)
    auto timeKeys = [&](int mode) {
        audio::AudioSystem engine(environment, audio::Deviceless{});
        engine.setPool(&environment.find<env::Scheduler>()->realtime());
        audio::MidiHub &midi = engine.add<audio::MidiHub>();
        daw::AudioBridge bridge(engine, midi, nullptr, library, pool);
        daw::AudioBridge::SyncOptions options;
        options.listen = false;
        daw::Project p;
        p.tracks.push_back(kickTrack("Kick"));
        for (int i = 0; i < 24; ++i)
            p.tracks.push_back(sourceTrack("S"));
        daw::ObjectId next = 1;
        p.normalizeIds(next);
        for (size_t i = 1; i < p.tracks.size() && mode > 0; ++i) {
            p.tracks[i].effects = {audio::effectSetting(audio::EffectType::Compressor)};
            if (mode == 2)
                p.tracks[i].effects[0].sidechain = p.tracks[0].id;
        }
        daw::ProjectStore store(p);
        bridge.sync(store, options);
        playFromStart(engine, 1.0); // warm
        const auto begin = std::chrono::steady_clock::now();
        playFromStart(engine, 8.0);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    };
    double bare = 1e30, compressors = 1e30, keyed = 1e30; // the fastest of three runs each, as above
    for (int run = 0; run < 3; ++run) {
        bare = std::min(bare, timeKeys(0));
        compressors = std::min(compressors, timeKeys(1));
        keyed = std::min(keyed, timeKeys(2));
    }
    std::printf("INFO sidechain bench: 4 s of 24 synth tracks and a kick: no effects %.1f ms, a compressor each %.1f ms, each keyed by the kick %.1f ms (%+.1f%% over unkeyed)\n",
                bare, compressors, keyed, (keyed / compressors - 1.0) * 100.0);
    c.check(true, "Routing bench");
}

}

int runRoutingChecks()
{
    std::printf("Routing self-test: sends, returns and groups\n\n");
    Checker c;
    modelChecks(c);
    foldChecks(c);
    foldProjectChecks(c);
    planChecks(c);
    flexibleModelChecks(c);
    flexiblePlanChecks(c);
    flexibleMenuChecks(c);
    sidechainModelChecks(c);
    sidechainPlanChecks(c);
    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    impulseChecks(c, environment);
    bridgeChecks(c, environment);
    flexibleEngineChecks(c, environment);
    masterAutomationChecks(c, environment);
    sidechainEngineChecks(c, environment);
    sidechainGateChecks(c, environment);
    sidechainDelayChecks(c, environment);
    benchChecks(c, environment);
    return c.finish("routing");
}

}
