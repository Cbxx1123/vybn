#include "Routing.h"

#include <algorithm>
#include <unordered_map>

#include "ProjectStore.h"
#include "audio/Effects.h"
#include "audio/Routing.h"

namespace daw {

namespace {

// The track indices by id
std::unordered_map<ObjectId, int> indexById(const Project &project)
{
    std::unordered_map<ObjectId, int> out;
    out.reserve(project.tracks.size());
    for (size_t i = 0; i < project.tracks.size(); ++i)
        out.emplace(project.tracks[i].id, static_cast<int>(i));
    return out;
}

int lookup(const std::unordered_map<ObjectId, int> &ids, ObjectId id)
{
    if (id == kNoId)
        return -1;
    const auto it = ids.find(id);
    return it == ids.end() ? -1 : it->second;
}

// The routes the rules allow out of track `i`: its output's group (-1 for the master, or one refused),
// each send's bus (-1 for one refused: no such track, not a return or group, itself) and the tracks its
// effects are keyed by (a key from no track, from itself, or on an effect that takes none, is left out)
struct Allowed {
    struct Key {
        size_t effect;
        int from;
    };
    int output = -1;
    std::vector<int> sends;
    std::vector<Key> keys;
};

Allowed allowed(const Project &project, const std::unordered_map<ObjectId, int> &ids, size_t i)
{
    const Track &t = project.tracks[i];
    Allowed a;
    const int out = lookup(ids, t.output);
    if (out >= 0 && out != static_cast<int>(i) && project.tracks[static_cast<size_t>(out)].kind == TrackKind::Group)
        a.output = out;
    a.sends.reserve(t.sends.size());
    for (const Send &s : t.sends) {
        const int to = lookup(ids, s.to);
        a.sends.push_back(to >= 0 && to != static_cast<int>(i) && isBus(project.tracks[static_cast<size_t>(to)].kind) ? to : -1);
    }
    for (size_t e = 0; e < t.effects.size(); ++e) {
        const int from = t.effects[e].sidechain != kNoId && mayTakeSidechain(t.effects[e]) ? lookup(ids, t.effects[e].sidechain) : -1;
        if (from >= 0 && from != static_cast<int>(i))
            a.keys.push_back({e, from});
    }
    return a;
}

// Every allowed route, as a list of destinations per track. A sidechain counts as one, from the
// track that keys to the track keyed: its sound isn't heard there, but it has to be rendered first.
std::vector<std::vector<int>> graphOf(const Project &project)
{
    const auto ids = indexById(project);
    std::vector<std::vector<int>> adj(project.tracks.size());
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Allowed a = allowed(project, ids, i);
        if (a.output >= 0)
            adj[i].push_back(a.output);
        for (int s : a.sends) {
            if (s >= 0)
                adj[i].push_back(s);
        }
        for (const Allowed::Key &k : a.keys)
            adj[static_cast<size_t>(k.from)].push_back(static_cast<int>(i));
    }
    return adj;
}

// Whether `to` is reached from `from` along `adj` (a node reaches itself)
bool reachesIn(const std::vector<std::vector<int>> &adj, int from, int to)
{
    if (from == to)
        return true;
    std::vector<char> seen(adj.size(), 0);
    std::vector<int> stack{from};
    seen[static_cast<size_t>(from)] = 1;
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        for (int next : adj[static_cast<size_t>(n)]) {
            if (next == to)
                return true;
            if (!seen[static_cast<size_t>(next)]) {
                seen[static_cast<size_t>(next)] = 1;
                stack.push_back(next);
            }
        }
    }
    return false;
}

// Marks everything reached from the marked nodes along `adj`
void spread(const std::vector<std::vector<int>> &adj, std::vector<char> &marked)
{
    std::vector<int> stack;
    for (size_t i = 0; i < marked.size(); ++i) {
        if (marked[i])
            stack.push_back(static_cast<int>(i));
    }
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        for (int next : adj[static_cast<size_t>(n)]) {
            if (!marked[static_cast<size_t>(next)]) {
                marked[static_cast<size_t>(next)] = 1;
                stack.push_back(next);
            }
        }
    }
}

// The groups track `i` goes through, nearest first: its output, that group's output, and so on. Stops
// at the master, at an output the rules refuse, and when a track comes round again.
std::vector<int> groupChain(const Project &project, const std::unordered_map<ObjectId, int> &ids, size_t i)
{
    std::vector<int> chain;
    int at = static_cast<int>(i);
    for (size_t step = 0; step < project.tracks.size(); ++step) {
        const int out = lookup(ids, project.tracks[static_cast<size_t>(at)].output);
        if (out < 0 || out == at || project.tracks[static_cast<size_t>(out)].kind != TrackKind::Group || out == static_cast<int>(i) ||
            std::find(chain.begin(), chain.end(), out) != chain.end())
            break;
        chain.push_back(out);
        at = out;
    }
    return chain;
}

std::string named(const Project &project, size_t i)
{
    return "track " + std::to_string(i + 1) + " (\"" + project.tracks[i].name + "\"): ";
}

}

namespace {
std::function<uint32_t(const std::string &)> gPluginSidechain;
}

void setPluginSidechainLookup(std::function<uint32_t(const std::string &)> lookup)
{
    gPluginSidechain = std::move(lookup);
}

bool mayTakeSidechain(const audio::DeviceSetting &device)
{
    audio::EffectType type;
    return device.kind == audio::DeviceKind::Plugin ||
           (device.kind == audio::DeviceKind::Effect && audio::effectFromKey(device.id, type) && audio::effectInfo(type).sidechain);
}

bool takesSidechain(const audio::DeviceSetting &device)
{
    if (device.kind == audio::DeviceKind::Plugin)
        return gPluginSidechain && gPluginSidechain(device.id) > 0;
    return mayTakeSidechain(device);
}

size_t keyedCount(const std::vector<audio::DeviceSetting> &effects)
{
    return static_cast<size_t>(std::count_if(effects.begin(), effects.end(), [](const audio::DeviceSetting &d) {
        return d.sidechain != kNoId && mayTakeSidechain(d);
    }));
}

SidechainOffer sidechainOffer(const Project &project, const DevicePlace &place)
{
    SidechainOffer offer;
    const audio::DeviceSetting *device = place.instrument() ? nullptr : project.device(place);
    if (!device || !takesSidechain(*device))
        return offer;
    offer.available = true;
    offer.current = device->sidechain;
    const std::vector<audio::DeviceSetting> *chain = project.effects(place.master, place.track);
    const size_t limit = place.master ? kMaxMasterKeys : kMaxTrackKeys;
    if (device->sidechain == kNoId && chain && keyedCount(*chain) >= limit) {
        offer.note = std::to_string(limit) + (place.master ? " sidechains on the master at most" : " sidechains per track at most");
        return offer;
    }
    // canSidechain for every track, with the routes worked out once
    const std::vector<std::vector<int>> adj = place.master ? std::vector<std::vector<int>>() : graphOf(project);
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &t = project.tracks[i];
        if (t.id == kNoId) // a track added this instant, with no id yet
            continue;
        if (place.master || (static_cast<int>(i) != place.track && !reachesIn(adj, place.track, static_cast<int>(i))) ||
            t.id == device->sidechain)
            offer.sources.push_back({t.id, t.name});
    }
    return offer;
}

std::string returnLetter(int index)
{
    std::string out;
    for (int n = std::max(0, index) + 1; n > 0; n = (n - 1) / 26)
        out.insert(out.begin(), static_cast<char>('A' + (n - 1) % 26));
    return out;
}

std::string returnLetterOf(const Project &project, int track)
{
    const Track *t = project.track(track);
    if (!t || t->kind != TrackKind::Return)
        return {};
    int before = 0;
    for (int i = 0; i < track; ++i)
        before += project.tracks[static_cast<size_t>(i)].kind == TrackKind::Return ? 1 : 0;
    return returnLetter(before);
}

const Send *findSend(const Track &track, ObjectId to)
{
    const auto it = std::find_if(track.sends.begin(), track.sends.end(), [to](const Send &s) { return s.to == to; });
    return it == track.sends.end() ? nullptr : &*it;
}

Send *findSend(Track &track, ObjectId to)
{
    return const_cast<Send *>(findSend(static_cast<const Track &>(track), to));
}

bool reaches(const Project &project, int from, int to)
{
    if (!project.track(from) || !project.track(to))
        return false;
    return reachesIn(graphOf(project), from, to);
}

bool canSidechain(const Project &project, int track, int source)
{
    return project.track(track) && project.track(source) && source != track && !reaches(project, track, source);
}

bool canOutputTo(const Project &project, int track, int group)
{
    if (!project.track(track))
        return false;
    if (group < 0)
        return true;
    const Track *g = project.track(group);
    // (not a group the track already sends to: it would be fed twice, as for canSendTo)
    return g && g->kind == TrackKind::Group && group != track && !findSend(*project.track(track), g->id) &&
           !reaches(project, group, track);
}

bool canSendTo(const Project &project, int track, int bus)
{
    const Track *t = project.track(track);
    const Track *b = project.track(bus);
    if (!t || !b || !isBus(b->kind) || bus == track || reaches(project, bus, track))
        return false;
    // A send to the group the track already outputs to would play it twice: refused. Past the cap
    // (audio::kMaxSends) no further send is offered.
    if (b->kind == TrackKind::Group && t->output == b->id)
        return false;
    return findSend(*t, b->id) != nullptr || t->sends.size() < audio::kMaxSends;
}

std::vector<int> outputChoices(const Project &project, int track)
{
    std::vector<int> out;
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        if (canOutputTo(project, track, static_cast<int>(i)))
            out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<int> sendChoices(const Project &project, int track)
{
    std::vector<int> out;
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        if (canSendTo(project, track, static_cast<int>(i)))
            out.push_back(static_cast<int>(i));
    }
    return out;
}

std::string outputName(const Project &project, int track)
{
    const Track *t = project.track(track);
    const Track *group = t && t->output != kNoId ? project.findTrack(t->output) : nullptr;
    return group && group->kind == TrackKind::Group ? group->name : std::string("Master");
}

std::vector<int> alsoToChoices(const Project &project, int track)
{
    std::vector<int> out;
    const Track *t = project.track(track);
    for (size_t i = 0; t && i < project.tracks.size(); ++i) {
        if (project.tracks[i].kind == TrackKind::Group && (findSend(*t, project.tracks[i].id) || canSendTo(project, track, static_cast<int>(i))))
            out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<int> groupSends(const Project &project, int track)
{
    std::vector<int> out;
    const Track *t = project.track(track);
    for (size_t s = 0; t && s < t->sends.size(); ++s) {
        const int to = project.trackIndex(t->sends[s].to);
        if (project.track(to) && project.tracks[static_cast<size_t>(to)].kind == TrackKind::Group)
            out.push_back(to);
    }
    return out;
}

void removeSend(Track &track, ObjectId to)
{
    track.sends.erase(std::remove_if(track.sends.begin(), track.sends.end(), [to](const Send &s) { return s.to == to; }), track.sends.end());
    const uint32_t param = sendParam(to);
    track.automation.erase(std::remove_if(track.automation.begin(), track.automation.end(),
                                          [param](const audio::AutomationLane &l) { return l.param == param; }),
                           track.automation.end());
}

void setAlsoTo(ProjectStore &store, int track, ObjectId group, bool on)
{
    const Project &project = store.project();
    const Track *t = project.track(track);
    const int g = project.trackIndex(group);
    if (!t || !project.track(g) || project.tracks[static_cast<size_t>(g)].kind != TrackKind::Group)
        return;
    const bool has = findSend(*t, group) != nullptr;
    if (on && !has && canSendTo(project, track, g)) {
        store.editMix(track).sends.push_back({group, 1.0f, false});
    } else if (!on && has) {
        // The send alone is a mix edit; with a lane to take too it is the track's
        if (audio::findAutomation(t->automation, sendParam(group)))
            removeSend(store.editTrack(track), group);
        else
            removeSend(store.editMix(track), group);
    }
}

std::string extraDestinations(const Project &project, int track)
{
    const size_t n = groupSends(project, track).size();
    return n ? " + " + std::to_string(n) : std::string();
}

std::string outputLabel(const Project &project, int track, size_t maxChars)
{
    const std::string extra = extraDestinations(project, track);
    std::string name = outputName(project, track);
    const size_t fixed = 3 + extra.size(); // "To " and the count
    if (maxChars > fixed + 2 && name.size() + fixed > maxChars)
        name = name.substr(0, maxChars - fixed - 2) + "..";
    return "To " + name + extra;
}

std::string groupSendLabel(const std::string &name)
{
    if (name.size() > 6 && name.compare(0, 6, "Group ") == 0 && name.find_first_not_of("0123456789", 6) == std::string::npos)
        return "G" + name.substr(6);
    return name.substr(0, 2);
}

std::string groupSendLabel(const Project &project, int group)
{
    const Track *t = project.track(group);
    if (!t)
        return {};
    const std::string base = groupSendLabel(t->name);
    if (t->kind != TrackKind::Group)
        return base;
    int ordinal = 0, alike = 0;
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &other = project.tracks[i];
        if (other.kind != TrackKind::Group || groupSendLabel(other.name) != base)
            continue;
        ++alike;
        if (static_cast<int>(i) <= group)
            ordinal = alike;
    }
    return alike > 1 ? base + std::to_string(ordinal) : base;
}

void forgetTrack(Project &project, ObjectId id)
{
    if (id == kNoId)
        return;
    auto unkey = [id](audio::DeviceSetting &device) {
        if (device.sidechain == id)
            device.sidechain = kNoId;
    };
    for (audio::DeviceSetting &d : project.masterEffects)
        unkey(d);
    for (Track &t : project.tracks) {
        unkey(t.instrument);
        for (audio::DeviceSetting &d : t.effects)
            unkey(d);
        if (t.output == id)
            t.output = kNoId;
        t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [id](const Send &s) { return s.to == id; }), t.sends.end());
        const uint32_t param = sendParam(id);
        t.automation.erase(std::remove_if(t.automation.begin(), t.automation.end(),
                                          [param](const audio::AutomationLane &l) { return l.param == param; }),
                           t.automation.end());
    }
}

std::string routingError(const Project &project)
{
    const auto ids = indexById(project);
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &t = project.tracks[i];
        if (t.output != kNoId) {
            const int out = lookup(ids, t.output);
            if (out < 0)
                return named(project, i) + "its output is track id " + std::to_string(t.output) + ", which no track has";
            if (out == static_cast<int>(i))
                return named(project, i) + "its output is itself";
            if (project.tracks[static_cast<size_t>(out)].kind != TrackKind::Group)
                return named(project, i) + "its output is \"" + project.tracks[static_cast<size_t>(out)].name +
                       "\", which isn't a group track";
        }
        if (t.sends.size() > audio::kMaxSends)
            return named(project, i) + "it has " + std::to_string(t.sends.size()) + " sends, at most " +
                   std::to_string(audio::kMaxSends) + " are allowed";
        for (size_t s = 0; s < t.sends.size(); ++s) {
            const int to = lookup(ids, t.sends[s].to);
            if (to < 0)
                return named(project, i) + "a send goes to track id " + std::to_string(t.sends[s].to) + ", which no track has";
            if (to == static_cast<int>(i))
                return named(project, i) + "a send goes to itself";
            if (!isBus(project.tracks[static_cast<size_t>(to)].kind))
                return named(project, i) + "a send goes to \"" + project.tracks[static_cast<size_t>(to)].name +
                       "\", which isn't a bus (a return or group track)";
            if (t.sends[s].to == t.output)
                return named(project, i) + "a send goes to \"" + project.tracks[static_cast<size_t>(to)].name +
                       "\", which is also its output";
            for (size_t e = 0; e < s; ++e) {
                if (t.sends[e].to == t.sends[s].to)
                    return named(project, i) + "two sends go to \"" + project.tracks[static_cast<size_t>(to)].name + "\"";
            }
        }
        // Sidechains: only on effects that take one, from a track, not the track itself
        if (t.instrument.sidechain != kNoId)
            return named(project, i) + "its instrument can't take a sidechain";
        if (keyedCount(t.effects) > kMaxTrackKeys)
            return named(project, i) + "it has " + std::to_string(keyedCount(t.effects)) + " sidechains, at most " +
                   std::to_string(kMaxTrackKeys) + " are allowed";
        for (const audio::DeviceSetting &d : t.effects) {
            if (d.sidechain == kNoId)
                continue;
            if (!mayTakeSidechain(d))
                return named(project, i) + "the effect \"" + d.name + "\" can't take a sidechain";
            const int from = lookup(ids, d.sidechain);
            if (from < 0)
                return named(project, i) + "a sidechain is track id " + std::to_string(d.sidechain) + ", which no track has";
            if (from == static_cast<int>(i))
                return named(project, i) + "a sidechain is the track itself";
        }
    }
    if (keyedCount(project.masterEffects) > kMaxMasterKeys)
        return "the master has " + std::to_string(keyedCount(project.masterEffects)) + " sidechains, at most " +
               std::to_string(kMaxMasterKeys) + " are allowed";
    for (const audio::DeviceSetting &d : project.masterEffects) {
        if (d.sidechain == kNoId)
            continue;
        if (!mayTakeSidechain(d))
            return "the master's effect \"" + d.name + "\" can't take a sidechain";
        if (lookup(ids, d.sidechain) < 0)
            return "the master's effect \"" + d.name + "\" has a sidechain, track id " + std::to_string(d.sidechain) +
                   ", which no track has";
    }
    const auto adj = graphOf(project);
    for (size_t i = 0; i < adj.size(); ++i) {
        for (int next : adj[i]) {
            if (reachesIn(adj, next, static_cast<int>(i)))
                return named(project, i) + "its sound comes back to it through \"" +
                       project.tracks[static_cast<size_t>(next)].name + "\"";
        }
    }
    return {};
}

RoutingPlan planRouting(const Project &project, const std::vector<uint32_t> &latencies, uint32_t maxDelay)
{
    const size_t n = project.tracks.size();
    const auto ids = indexById(project);
    RoutingPlan plan;
    plan.nodes.resize(n);

    // The routes, in track order, each dropped if it would close a loop with those taken before it.
    // `adj` is every edge that orders the tracks (sounds and keys); `sound` only where sound goes (solo).
    std::vector<std::vector<int>> adj(n), sound(n);
    std::vector<std::vector<Allowed::Key>> keyed(n); // the keys kept, by the track they key
    for (size_t i = 0; i < n; ++i) {
        RoutingPlan::Node &node = plan.nodes[i];
        const Allowed a = allowed(project, ids, i);
        if (a.output >= 0 && !reachesIn(adj, a.output, static_cast<int>(i))) {
            node.output.to = a.output;
            adj[i].push_back(a.output);
            sound[i].push_back(a.output);
        }
        node.sends.resize(a.sends.size());
        for (size_t s = 0; s < a.sends.size(); ++s) {
            const int to = a.sends[s];
            if (to < 0 || std::find(adj[i].begin(), adj[i].end(), to) != adj[i].end() || reachesIn(adj, to, static_cast<int>(i)))
                continue;
            node.sends[s].to = to;
            adj[i].push_back(to);
            sound[i].push_back(to);
        }
        // A key is an edge from its source to this track: dropped if this track already reaches the source
        for (const Allowed::Key &k : a.keys) {
            if (reachesIn(adj, static_cast<int>(i), k.from))
                continue;
            keyed[i].push_back(k);
            std::vector<int> &edges = adj[static_cast<size_t>(k.from)];
            if (std::find(edges.begin(), edges.end(), static_cast<int>(i)) == edges.end())
                edges.push_back(static_cast<int>(i));
        }
    }

    // Stages and times, in an order where every track comes after all that feed it (no loops are left)
    std::vector<int> indegree(n, 0);
    for (const auto &out : adj) {
        for (int to : out)
            ++indegree[static_cast<size_t>(to)];
    }
    std::vector<int> order;
    order.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0)
            order.push_back(static_cast<int>(i));
    }
    std::vector<uint64_t> in(n, 0), out(n, 0); // when a track's input lines up, and when its sound leaves
    for (size_t k = 0; k < order.size(); ++k) {
        const size_t i = static_cast<size_t>(order[k]);
        out[i] = in[i] + (i < latencies.size() ? latencies[i] : 0u);
        plan.stages = std::max(plan.stages, plan.nodes[i].stage + 1);
        for (int to : adj[i]) {
            const size_t j = static_cast<size_t>(to);
            plan.nodes[j].stage = std::max(plan.nodes[j].stage, plan.nodes[i].stage + 1);
            in[j] = std::max(in[j], out[i]);
            if (--indegree[j] == 0)
                order.push_back(to);
        }
    }
    uint64_t master = 0;
    for (size_t i = 0; i < n; ++i) {
        if (plan.nodes[i].output.to < 0)
            master = std::max(master, out[i]);
    }
    plan.masterTime = static_cast<uint32_t>(std::min<uint64_t>(master, UINT32_MAX));
    auto delay = [maxDelay](uint64_t needed, uint64_t arrives) {
        return static_cast<uint32_t>(std::min<uint64_t>(needed > arrives ? needed - arrives : 0, maxDelay));
    };
    for (size_t i = 0; i < n; ++i) {
        RoutingPlan::Node &node = plan.nodes[i];
        node.output.delay = delay(node.output.to < 0 ? master : in[static_cast<size_t>(node.output.to)], out[i]);
        for (RoutingPlan::Path &send : node.sends) {
            if (send.to >= 0)
                send.delay = delay(in[static_cast<size_t>(send.to)], out[i]);
        }
        // A key is held back so it lines up with the keyed track's input
        for (const Allowed::Key &k : keyed[i])
            node.keys.push_back({k.effect, k.from, delay(in[i], out[static_cast<size_t>(k.from)])});
    }
    // The master's effects keyed by tracks: they run after every stage, at the master's time
    for (size_t e = 0; e < project.masterEffects.size(); ++e) {
        const audio::DeviceSetting &d = project.masterEffects[e];
        const int from = d.sidechain != kNoId && mayTakeSidechain(d) ? lookup(ids, d.sidechain) : -1;
        if (from >= 0)
            plan.masterKeys.push_back({e, from, delay(master, out[static_cast<size_t>(from)])});
    }

    // Solo: the soloed tracks and what feeds them, then everything those feed
    std::vector<char> heard(n, 0);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        heard[i] = project.tracks[i].solo ? 1 : 0;
        any = any || heard[i];
    }
    if (any) {
        std::vector<std::vector<int>> back(n);
        for (size_t i = 0; i < n; ++i) {
            for (int to : sound[i])
                back[static_cast<size_t>(to)].push_back(static_cast<int>(i));
        }
        spread(back, heard);
        spread(sound, heard);
        for (size_t i = 0; i < n; ++i)
            plan.nodes[i].soloed = heard[i] != 0;
    }
    return plan;
}

std::vector<int> groupDepth(const Project &project)
{
    const auto ids = indexById(project);
    std::vector<int> depth(project.tracks.size(), 0);
    for (size_t i = 0; i < depth.size(); ++i)
        depth[i] = static_cast<int>(groupChain(project, ids, i).size());
    return depth;
}

std::vector<int> outputGroups(const Project &project, int track)
{
    if (!project.track(track))
        return {};
    return groupChain(project, indexById(project), static_cast<size_t>(track));
}

bool insideGroup(const Project &project, int track, int group)
{
    if (!project.track(track) || !project.track(group))
        return false;
    const auto ids = indexById(project);
    const std::vector<int> chain = groupChain(project, ids, static_cast<size_t>(track));
    return std::find(chain.begin(), chain.end(), group) != chain.end();
}

std::vector<int> dragBlock(const Project &project, int track)
{
    std::vector<int> block;
    const Track *t = project.track(track);
    if (!t)
        return block;
    if (t->kind != TrackKind::Group || !t->folded) {
        block.push_back(track);
        return block;
    }
    const auto ids = indexById(project);
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const std::vector<int> chain = groupChain(project, ids, i);
        if (static_cast<int>(i) == track || std::find(chain.begin(), chain.end(), track) != chain.end())
            block.push_back(static_cast<int>(i));
    }
    return block;
}

namespace {

// The tracks not in the block, in order
std::vector<int> restOf(const Project &project, const std::vector<int> &block)
{
    std::vector<char> in(project.tracks.size(), 0);
    for (int b : block) {
        if (b >= 0 && static_cast<size_t>(b) < in.size())
            in[static_cast<size_t>(b)] = 1;
    }
    std::vector<int> rest;
    for (size_t i = 0; i < in.size(); ++i) {
        if (!in[i])
            rest.push_back(static_cast<int>(i));
    }
    return rest;
}

}

int settleDrop(const Project &project, const std::vector<int> &block, int slot)
{
    const std::vector<int> rest = restOf(project, block);
    const int n = static_cast<int>(rest.size());
    slot = std::clamp(slot, 0, n);
    if (slot == n)
        return slot;
    const std::vector<char> hidden = hiddenTracks(project);
    if (!hidden[static_cast<size_t>(rest[static_cast<size_t>(slot)])])
        return slot; // before a track that shows: nothing is split
    // The run of hidden tracks (in the order left) the slot is in front of one of
    int first = slot, last = slot;
    while (first > 0 && hidden[static_cast<size_t>(rest[static_cast<size_t>(first - 1)])])
        --first;
    while (last + 1 < n && hidden[static_cast<size_t>(rest[static_cast<size_t>(last + 1)])])
        ++last;
    const int after = last + 1;
    // Before the folded group the run belongs to, or, when what precedes it isn't one, before the run
    int before = first;
    if (first > 0) {
        const Track &owner = project.tracks[static_cast<size_t>(rest[static_cast<size_t>(first - 1)])];
        if (owner.kind == TrackKind::Group && owner.folded)
            before = first - 1;
    }
    return slot - before < after - slot ? before : after;
}

std::vector<int> dropOrder(const Project &project, const std::vector<int> &block, int slot)
{
    const std::vector<int> rest = restOf(project, block);
    slot = std::clamp(slot, 0, static_cast<int>(rest.size()));
    std::vector<int> order(rest.begin(), rest.begin() + slot);
    order.insert(order.end(), block.begin(), block.end());
    order.insert(order.end(), rest.begin() + slot, rest.end());
    return order;
}

void moveBlock(Project &project, const std::vector<int> &block, int slot)
{
    if (block.empty())
        return;
    const std::vector<int> order = dropOrder(project, block, slot);
    if (order.size() != project.tracks.size())
        return;
    const ObjectId selected = project.track(project.selectedTrack) ? project.tracks[static_cast<size_t>(project.selectedTrack)].id : kNoId;
    std::vector<Track> moved;
    moved.reserve(order.size());
    for (int i : order)
        moved.push_back(std::move(project.tracks[static_cast<size_t>(i)]));
    project.tracks = std::move(moved);
    if (selected != kNoId)
        project.selectedTrack = project.trackIndex(selected);
}

std::vector<char> hiddenTracks(const Project &project)
{
    std::set<ObjectId> folded;
    for (const Track &t : project.tracks) {
        if (t.kind == TrackKind::Group && t.folded)
            folded.insert(t.id);
    }
    return hiddenTracks(project, folded);
}

std::vector<char> hiddenTracks(const Project &project, const std::set<ObjectId> &folded)
{
    std::vector<char> hidden(project.tracks.size(), 0);
    if (folded.empty())
        return hidden;
    const auto ids = indexById(project);
    for (size_t i = 0; i < hidden.size(); ++i) {
        for (int g : groupChain(project, ids, i)) {
            if (folded.count(project.tracks[static_cast<size_t>(g)].id)) {
                hidden[i] = 1;
                break;
            }
        }
    }
    return hidden;
}

}
