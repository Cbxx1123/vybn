#include "daw/audio/AudioBridge.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "daw/model/AutomationLanes.h"
#include "daw/widgets/Format.h"
#include "daw/model/Routing.h"
#include "daw/model/Warp.h"
#include "audio/DeviceChain.h"
#include "audio/PluginInstrument.h"
#include "plugin/Slot.h"

namespace daw {

namespace {

// A note's expressions reach the instruments by number
static_assert(static_cast<int>(Expression::Tuning) == audio::Instrument::kTuning && kExpressions == audio::Instrument::kExpressions,
              "daw::Expression and audio::Instrument number the expressions alike");

// FNV-1a over the clips a track plays and their notes, to notice edits without tracking every one
uint64_t hashNotes(const std::vector<Clip> &clips)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *data, size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const Clip &c : clips) {
        mix(&c.start, sizeof(c.start));
        mix(&c.length, sizeof(c.length));
        for (const Note &n : c.notes) {
            mix(&n.start, sizeof(n.start));
            mix(&n.length, sizeof(n.length));
            mix(&n.key, sizeof(n.key));
            mix(&n.velocity, sizeof(n.velocity));
            mix(&n.muted, sizeof(n.muted));
            for (const NoteCurve &curve : n.expression) {
                mix(&curve.type, sizeof(curve.type));
                for (const ControlPoint &p : curve.points) {
                    mix(&p.beat, sizeof(p.beat));
                    mix(&p.value, sizeof(p.value));
                }
            }
        }
        for (const ControlLane &lane : c.controls) {
            mix(&lane.controller, sizeof(lane.controller));
            for (const ControlPoint &p : lane.points) {
                mix(&p.beat, sizeof(p.beat));
                mix(&p.value, sizeof(p.value));
            }
        }
    }
    return h;
}

// The notes of the clips a track plays on the timeline, cut at the end of their clips
std::unique_ptr<audio::NoteSequence> sequenceFor(const std::vector<Clip> &clips)
{
    auto sequence = std::make_unique<audio::NoteSequence>();
    for (const Clip &c : clips) {
        for (const Note &n : c.notes) {
            if (n.start >= c.length || n.length <= 0.0 || n.muted)
                continue;
            const double start = c.start + n.start;
            const double end = std::min(start + n.length, c.start + c.length);
            audio::SequencedNote played{start, end, n.key, n.velocity, 0, 0};
            // Its expression: the curves, by the instruments' numbering of them
            played.firstCurve = static_cast<uint32_t>(sequence->curves.size());
            for (const NoteCurve &curve : n.expression) {
                if (curve.points.empty())
                    continue;
                audio::ExpressionCurve expression;
                expression.expression = static_cast<int>(curve.type);
                for (const ControlPoint &p : curve.points)
                    expression.points.push_back({p.beat, p.value});
                sequence->curves.push_back(std::move(expression));
            }
            played.curveCount = static_cast<uint32_t>(sequence->curves.size()) - played.firstCurve;
            sequence->notes.push_back(played);
        }
    }
    // Controller lanes: each controller's points from every clip on the timeline, in order;
    // a clip's curve ends where it does, holding its last value there
    for (const Clip &c : clips) {
        for (const ControlLane &lane : c.controls) {
            if (lane.points.empty())
                continue;
            auto curve = std::find_if(sequence->controls.begin(), sequence->controls.end(),
                                      [&lane](const audio::ControlCurve &k) { return k.controller == lane.controller; });
            if (curve == sequence->controls.end()) {
                sequence->controls.push_back({});
                curve = sequence->controls.end() - 1;
                curve->controller = lane.controller;
                curve->stepped = controllerStepped(lane.controller);
            }
            for (const ControlPoint &p : lane.points) {
                if (p.beat <= c.length)
                    curve->points.push_back({c.start + p.beat, p.value});
            }
            curve->points.push_back({c.start + c.length, lane.valueAt(c.length)});
        }
    }
    for (audio::ControlCurve &curve : sequence->controls)
        std::stable_sort(curve.points.begin(), curve.points.end(),
                         [](const audio::ControlCurve::Point &a, const audio::ControlCurve::Point &b) { return a.beat < b.beat; });
    return sequence;
}

void mix(uint64_t &h, const void *data, size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
}

}

AudioBridge::AudioBridge(audio::AudioSystem &audio, audio::MidiHub &midi, plugin::PluginManager *plugins,
                         InstrumentLibrary &library, AudioPool &pool)
    : audio_(audio), midi_(midi), plugins_(plugins), library_(library), pool_(pool)
{
}

AudioBridge::TrackAudio AudioBridge::makeTrack(const Track &t)
{
    // Instrument tracks play their notes (and MIDI keyboards) through a sampler; audio tracks
    // play their clips and monitor the input
    std::unique_ptr<audio::AudioProcessor> source;
    TrackAudio fresh;
    audio::ChainInstrument *chainInstrument = nullptr;
    if (t.kind == TrackKind::Instrument) {
        // Whatever instrument the track's chain has
        auto instrument = std::make_unique<audio::ChainInstrument>();
        chainInstrument = instrument.get();
        auto notes = std::make_unique<audio::NotePlayer>(std::move(instrument));
        notes->setMidiSource(&midi_);
        fresh.player = notes.get();
        source = std::move(notes);
    } else if (!isBus(t.kind)) {
        // (buses make no sound of their own: they play what they are given)
        auto audioClips = std::make_unique<audio::AudioClipPlayer>();
        fresh.clips = audioClips.get();
        source = std::move(audioClips);
    }
    fresh.processor = &audio_.add<audio::TrackProcessor>(t.name, solo_, std::move(source));
    fresh.processor->chain.setPlugins(plugins_);
    if (chainInstrument)
        chainInstrument->setChain(&fresh.processor->chain);
    fresh.id = t.id;
    fresh.kind = t.kind;
    ++made_;
    return fresh;
}

void AudioBridge::removeAudio(TrackAudio &state)
{
    // The processor leaves the graph (the audio thread lets it go, and tick deletes it); nothing
    // else in the state refers to it once it is gone
    if (state.processor)
        audio_.remove(*state.processor);
    state.processor = nullptr;
    state.player = nullptr;
    state.clips = nullptr;
    state.sampler = nullptr;
    state.pendingSet = nullptr;
    ++removed_;
}

void AudioBridge::reconcile(const Project &project)
{
    // Each project track takes the entry with its id and kind (its processor, players, chain and
    // loaded instrument, wherever it was), or a new one; the entries nothing took are removed
    std::vector<TrackAudio> next;
    next.reserve(project.tracks.size());
    std::vector<bool> taken(tracks_.size(), false);
    for (const Track &t : project.tracks) {
        size_t match = tracks_.size();
        for (size_t j = 0; j < tracks_.size(); ++j) {
            if (!taken[j] && tracks_[j].id == t.id && tracks_[j].kind == t.kind) {
                match = j;
                break;
            }
        }
        if (match < tracks_.size()) {
            taken[match] = true;
            next.push_back(std::move(tracks_[match]));
        } else {
            next.push_back(makeTrack(t));
        }
    }
    for (size_t j = 0; j < tracks_.size(); ++j) {
        if (!taken[j])
            retiring_.push_back(std::move(tracks_[j]));
    }
    tracks_ = std::move(next);
}

// The tempo points, which place audio clips in time (the signatures don't)
uint64_t tempoHash(const audio::TempoMap &timing)
{
    uint64_t h = 1469598103934665603ull;
    for (const audio::TempoPoint &p : timing.points()) {
        mix(h, &p.beat, sizeof(p.beat));
        mix(h, &p.bpm, sizeof(p.bpm));
        mix(h, &p.ramp, sizeof(p.ramp));
    }
    return h;
}

void AudioBridge::sync(ProjectStore &store, const SyncOptions &options)
{
    // Every track and clip gets its id first, so the tracks can be matched to the audio side by id
    store.normalizeIds();
    const Project &project = store.project();
    // The tempo map (published only when it changed)
    audio_.transport().setTiming(project.timing);
    const uint64_t tempo = tempoHash(project.timing);
    reconcile(project);
    levels_.resize(tracks_.size());
    // The master effects
    audio::DeviceChain &master = audio_.masterEffects();
    master.apply(project.masterEffects);
    master.collect();
    const uint64_t masterKey = automationKey(master, store.masterRevision());
    if (masterKey != masterAutomationKey_) {
        syncAutomation(master, nullptr, project.masterEffects, &project.masterAutomation);
        masterAutomationKey_ = masterKey;
    }
    for (size_t j = 0; j < project.masterEffects.size(); ++j)
        syncDevice(store, DevicePlace::effect(true, 0, static_cast<int>(j)), master.effect(j));
    audio_.masterVolume().set(project.masterVolume);
    // MIDI keyboards play the armed instrument tracks, or the selected one when none is armed
    bool instrumentArmed = false;
    for (const Track &t : project.tracks)
        instrumentArmed = instrumentArmed || (t.armed && t.kind == TrackKind::Instrument);
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const Track &t = project.tracks[i];
        TrackAudio &state = tracks_[i];
        audio::TrackProcessor &p = *state.processor;
        p.gain.set(t.gain);
        p.pan.set(t.pan);
        p.mute = t.mute;
        // (its solo comes from the routing: what a soloed track feeds, or is fed by, is heard too)
        for (size_t k = 0; k < t.sends.size() && k < audio::TrackProcessor::kMaxSends; ++k)
            p.setSendLevel(static_cast<uint32_t>(k), t.sends[k].level);
        // Its devices: the instrument (instrument tracks) and the effects, and the plugins' own
        // changes back into the project
        static const audio::DeviceSetting kNone;
        p.chain.apply(t.kind == TrackKind::Instrument ? t.instrument : kNone, t.effects);
        p.chain.collect();
        // Its automation: the curves made again when the track's content or its devices moved
        const uint64_t key = automationKey(p.chain, store.trackRevision(static_cast<int>(i)));
        if (key != state.automationKey) {
            syncAutomation(p.chain, t.kind == TrackKind::Instrument ? &t.instrument : nullptr, t.effects, &t.automation);
            state.automationKey = key;
        }
        if (t.kind == TrackKind::Instrument && t.hasInstrument())
            syncDevice(store, DevicePlace::instrumentOf(static_cast<int>(i)), p.chain.instrument());
        for (size_t j = 0; j < t.effects.size(); ++j)
            syncDevice(store, DevicePlace::effect(false, static_cast<int>(i), static_cast<int>(j)), p.chain.effect(j));
        levels_[i] = std::max(p.takePeak(0), p.takePeak(1));

        if (audio::AudioClipPlayer *clips = state.clips) {
            // The clips are looked at again only when the track, the pool or the tempo map moved. A clip
            // sequence not sent yet (the queue is full) is sent again next frame, as before.
            const uint64_t trackRevision = store.trackRevision(static_cast<int>(i));
            const uint64_t poolVersion = pool_.version();
            if (trackRevision != state.clipsRevision || poolVersion != state.clipsPoolVersion ||
                tempo != state.clipsTempo || auditionVersion_ != state.clipsAudition) {
                const std::vector<Clip> &played = playedClips(t);
                const uint64_t hash = hashClips(project, played);
                if (hash == state.clipHash || clips->setSequence(clipSequenceFor(project, played))) {
                    state.clipHash = hash;
                    state.clipsRevision = trackRevision;
                    state.clipsPoolVersion = poolVersion;
                    state.clipsTempo = tempo;
                    state.clipsAudition = auditionVersion_;
                }
            }
            clips->monitor = t.armed && t.monitor && t.inputChannel >= 0;
            clips->inputChannel = std::max(0, t.inputChannel);
            clips->inputStereo = t.inputStereo;
            clips->collect();
        }

        audio::NotePlayer *player = state.player;
        if (!player)
            continue;
        player->listen = options.listen && (instrumentArmed ? t.armed : static_cast<int>(i) == project.selectedTrack);
        // Notes: a fresh sequence whenever they change. The notes are only hashed when the track's
        // revision moved (a selection-only change moves it too, and the hash ignores it). If the
        // audio thread can't take a sequence yet, the revision stays stale and it's tried again next frame.
        const uint64_t trackRevision = store.trackRevision(static_cast<int>(i));
        if (trackRevision != state.notesRevision || auditionVersion_ != state.notesAudition) {
            const std::vector<Clip> &played = playedClips(t);
            const uint64_t hash = hashNotes(played);
            const bool unchanged = hash == state.noteHash;
            if (unchanged || player->setSequence(sequenceFor(played))) {
                sequences_ += unchanged ? 0 : 1;
                state.noteHash = hash;
                state.notesRevision = trackRevision;
                state.notesAudition = auditionVersion_;
            }
        }
        // A library instrument: loaded in the background, then handed to its sampler (asked
        // again when the sampler is a new one, so an instrument chosen again finds its samples)
        auto *sampler = dynamic_cast<audio::SamplerDevice *>(p.chain.instrument());
        const std::string wanted = sampler ? sampler->instrumentName() : std::string();
        if (wanted != state.requested || sampler != state.sampler) {
            state.requested = wanted;
            state.sampler = sampler;
            state.error.clear();
            state.pendingSet = nullptr;
            if (!wanted.empty()) {
                const std::string name = wanted;
                // Found again by id when it arrives: the track may have moved in the list since
                const ObjectId id = state.id;
                const TrackKind kind = state.kind;
                library_.load(name, [this, id, kind, name](std::shared_ptr<const audio::SampleSet> set, const std::string &error) {
                    TrackAudio *track = nullptr;
                    for (TrackAudio &s : tracks_) {
                        if (s.id == id && s.kind == kind && s.requested == name) {
                            track = &s;
                            break;
                        }
                    }
                    if (!track)
                        return; // the track has gone, moved on, or the project was replaced
                    if (set)
                        track->pendingSet = std::move(set);
                    else
                        track->error = error;
                });
            }
        }
        if (sampler && state.pendingSet && sampler->sampler().setSampleSet(state.pendingSet))
            state.pendingSet = nullptr;
        player->collect();
    }

    // The routing and delay compensation: every path held back to line up with the one its plugins
    // delay most
    for (size_t i = 0; i < tracks_.size(); ++i)
        tracks_[i].latency = trackLatency(i);
    syncRouting(project);
}

void AudioBridge::syncRouting(const Project &project)
{
    // What the routing is made from: the processors, the kinds, outputs, sends, solos and latencies
    uint64_t key = 1469598103934665603ull;
    const size_t count = tracks_.size();
    mix(key, &count, sizeof count);
    for (size_t i = 0; i < count; ++i) {
        const Track &t = project.tracks[i];
        const uintptr_t address = reinterpret_cast<uintptr_t>(tracks_[i].processor);
        mix(key, &address, sizeof address);
        mix(key, &t.kind, sizeof t.kind);
        mix(key, &t.output, sizeof t.output);
        mix(key, &t.solo, sizeof t.solo);
        mix(key, &tracks_[i].latency, sizeof tracks_[i].latency);
        for (const Send &s : t.sends) {
            mix(key, &s.to, sizeof s.to);
            mix(key, &s.preFader, sizeof s.preFader);
        }
        for (const audio::DeviceSetting &d : t.effects)
            mix(key, &d.sidechain, sizeof d.sidechain);
    }
    for (const audio::DeviceSetting &d : project.masterEffects)
        mix(key, &d.sidechain, sizeof d.sidechain);
    if (key == routingKey_ && retiring_.empty())
        return;
    routingKey_ = key;

    std::vector<uint32_t> latencies;
    latencies.reserve(count);
    for (const TrackAudio &t : tracks_)
        latencies.push_back(t.latency);
    const RoutingPlan plan = planRouting(project, latencies, audio::TrackProcessor::kMaxCompensation);
    auto routing = std::make_unique<audio::Routing>();
    for (int stage = 0; stage < plan.stages; ++stage) {
        routing->beginStage();
        for (size_t i = 0; i < count; ++i) {
            const RoutingPlan::Node &node = plan.nodes[i];
            if (node.stage != stage)
                continue;
            const Track &t = project.tracks[i];
            audio::Route route;
            route.track = tracks_[i].processor;
            route.output = node.output.to >= 0 ? tracks_[static_cast<size_t>(node.output.to)].processor : nullptr;
            for (size_t k = 0; k < node.sends.size() && k < audio::TrackProcessor::kMaxSends; ++k) {
                const RoutingPlan::Path &path = node.sends[k];
                if (path.to < 0)
                    continue;
                route.sends.push_back({tracks_[static_cast<size_t>(path.to)].processor, static_cast<uint32_t>(k),
                                       sendParam(t.sends[k].to), path.delay, t.sends[k].preFader});
            }
            for (const RoutingPlan::Node::Key &k : node.keys) {
                if (route.keys.size() >= audio::TrackProcessor::kMaxKeys)
                    break;
                route.keys.push_back({tracks_[static_cast<size_t>(k.from)].processor, static_cast<uint32_t>(k.effect), k.delay});
            }
            routing->add(std::move(route));
        }
    }
    for (const RoutingPlan::Node::Key &k : plan.masterKeys)
        routing->masterKeys.push_back({tracks_[static_cast<size_t>(k.from)].processor, static_cast<uint32_t>(k.effect), k.delay});
    for (size_t i = 0; i < count; ++i) {
        tracks_[i].processor->compensation = plan.nodes[i].output.delay;
        tracks_[i].processor->setSolo(plan.nodes[i].soloed);
    }
    masterTime_ = plan.masterTime;
    audio_.setRouting(std::move(routing));

    // Nothing the audio thread plays names them now
    for (TrackAudio &t : retiring_)
        removeAudio(t);
    retiring_.clear();
}

audio::TrackProcessor *AudioBridge::processor(int track) const
{
    if (track < 0 || static_cast<size_t>(track) >= tracks_.size())
        return nullptr;
    return tracks_[static_cast<size_t>(track)].processor;
}

void AudioBridge::dropAll()
{
    // The tracks leave the graph; sync adds the project's. Loads still under way for the old
    // tracks find no entry with their id and kind waiting for them, or one that asked for the same
    // instrument, which gets the same samples, and are dropped otherwise.
    audio_.setRouting(nullptr);
    routingKey_ = 0;
    for (TrackAudio &t : tracks_)
        removeAudio(t);
    for (TrackAudio &t : retiring_)
        removeAudio(t);
    tracks_.clear();
    retiring_.clear();
    levels_.clear();
}

void AudioBridge::captureStates(ProjectStore &store)
{
    // Its state now, written into the project only when it differs from what the project has
    auto capture = [&store](audio::Device *device, const DevicePlace &place) {
        const audio::DeviceSetting *now = store.project().device(place);
        plugin::Slot *slot = device && now && device->matches(*now) ? device->slot() : nullptr;
        if (!slot)
            return;
        auto state = slot->captureState();
        if (!state || (now->state && *now->state == *state))
            return;
        if (audio::DeviceSetting *setting = store.editDevice(place))
            setting->state = state;
    };
    const Project &project = store.project();
    for (size_t j = 0; j < project.masterEffects.size(); ++j)
        capture(audio_.masterEffects().effect(j), DevicePlace::effect(true, 0, static_cast<int>(j)));
    // Each project track's audio side is the entry with its id and kind (a save can come before
    // this frame's sync, when the list is still in the order it had)
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &t = project.tracks[i];
        const TrackAudio *state = nullptr;
        for (const TrackAudio &s : tracks_) {
            if (s.id == t.id && s.kind == t.kind) {
                state = &s;
                break;
            }
        }
        if (!state || !state->processor)
            continue;
        if (t.kind == TrackKind::Instrument && t.hasInstrument())
            capture(state->processor->chain.instrument(), DevicePlace::instrumentOf(static_cast<int>(i)));
        for (size_t j = 0; j < t.effects.size(); ++j)
            capture(state->processor->chain.effect(j), DevicePlace::effect(false, static_cast<int>(i), static_cast<int>(j)));
    }
}

void AudioBridge::reloadInstrument(const std::string &name)
{
    // Tracks that asked for it before it was installed load it now
    for (TrackAudio &t : tracks_) {
        if (t.requested == name)
            t.requested = std::string(1, '\0');
    }
}

bool AudioBridge::instrumentFailed(size_t track) const
{
    return track < tracks_.size() && !tracks_[track].error.empty();
}

bool AudioBridge::instrumentReady(size_t track, const std::string &name) const
{
    return track < tracks_.size() && tracks_[track].requested == name && !tracks_[track].pendingSet && !library_.loading(name);
}

std::string AudioBridge::instrumentStatus(const Project &project, int index) const
{
    const Track *t = project.track(index);
    if (!t)
        return {};
    if (t->kind != TrackKind::Instrument)
        return "Audio tracks have no instrument";
    if (!t->hasInstrument())
        return "No instrument: the notes are silent";
    if (t->instrument.kind != audio::DeviceKind::Sampler)
        return {};
    const std::string &name = t->instrument.id;
    const auto &names = library_.instruments();
    if (std::find(names.begin(), names.end(), name) == names.end())
        return "\"" + name + "\" isn't in the library";
    if (library_.loading(name))
        return "Loading...";
    if (static_cast<size_t>(index) < tracks_.size() && !tracks_[static_cast<size_t>(index)].error.empty())
        return tracks_[static_cast<size_t>(index)].error;
    if (auto set = library_.cached(name)) {
        // In memory, and what streams from disk
        std::string text = format("%.0f samples", static_cast<double>(set->zones.size())) + format(",  %.1f MB", set->bytes() / 1e6);
        if (const uint64_t streamed = set->streamedBytes())
            text += format(" + %.0f MB on disk", streamed / 1e6);
        return text;
    }
    return {};
}

audio::DeviceChain *AudioBridge::chainOf(bool master, int track)
{
    if (master)
        return &audio_.masterEffects();
    return track >= 0 && static_cast<size_t>(track) < tracks_.size() ? &tracks_[static_cast<size_t>(track)].processor->chain : nullptr;
}

audio::Device *AudioBridge::deviceAt(const Project &project, const DevicePlace &place)
{
    audio::DeviceChain *chain = chainOf(place.master, place.track);
    audio::Device *device = chain ? chain->device(place.index) : nullptr;
    // Only the device made for what the project has there now (the chain catches up each frame)
    const audio::DeviceSetting *setting = project.device(place);
    return device && setting && device->matches(*setting) ? device : nullptr;
}

std::vector<plugin::Slot *> AudioBridge::pluginSlots()
{
    std::vector<plugin::Slot *> out;
    auto collect = [&](audio::DeviceChain &chain) {
        if (chain.instrument())
            if (plugin::Slot *slot = chain.instrument()->slot())
                out.push_back(slot);
        for (size_t i = 0; i < chain.size(); ++i)
            if (plugin::Slot *slot = chain.effect(i)->slot())
                out.push_back(slot);
    };
    collect(audio_.masterEffects());
    for (const TrackAudio &t : tracks_)
        collect(t.processor->chain);
    return out;
}

uint32_t AudioBridge::trackLatency(size_t track) const
{
    // Its instrument's and its effects' (a bypassed plugin isn't run, so it delays nothing)
    return track < tracks_.size() ? tracks_[track].processor->chain.latency() : 0;
}

std::string AudioBridge::latencyText(int track) const
{
    if (track < 0 || static_cast<size_t>(track) >= tracks_.size())
        return {};
    const TrackAudio &state = tracks_[static_cast<size_t>(track)];
    if (masterTime_ == 0)
        return {};
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    const uint32_t held = state.processor->compensation.load(std::memory_order_relaxed);
    const std::string text = format("Plugin latency %.1f ms", state.latency * 1000.0 / rate);
    return held == 0 ? text : text + format(", held %.1f ms to line up", held * 1000.0 / rate);
}

audio::NotePlayer *AudioBridge::player(int track) const
{
    return track >= 0 && static_cast<size_t>(track) < tracks_.size() ? tracks_[static_cast<size_t>(track)].player : nullptr;
}

void AudioBridge::releaseKey(int key)
{
    // Every player, in case the selection moved while the key was held
    for (const TrackAudio &t : tracks_) {
        if (t.player)
            t.player->release(key);
    }
}

uint64_t AudioBridge::automationKey(audio::DeviceChain &chain, uint64_t revision)
{
    uint64_t h = 1469598103934665603ull;
    mix(h, &revision, sizeof revision);
    auto device = [&h](audio::Device *d) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(d);
        mix(h, &address, sizeof address);
        if (plugin::Slot *slot = d ? d->slot() : nullptr) {
            const uint64_t version = slot->paramsVersion();
            mix(h, &version, sizeof version);
        }
    };
    device(chain.instrument());
    for (size_t i = 0; i < chain.size(); ++i)
        device(chain.effect(i));
    return h;
}

void AudioBridge::syncAutomation(audio::DeviceChain &chain, const audio::DeviceSetting *instrument,
                                 const std::vector<audio::DeviceSetting> &effects,
                                 const std::vector<audio::AutomationLane> *mixLanes)
{
    std::vector<std::pair<const audio::Device *, audio::AutomationCurves>> devices;
    // A device's lanes with points, each with its parameter's range and curve (a plugin's parameters
    // are known once it has loaded: until then its lanes wait)
    auto add = [&devices](audio::Device *device, const audio::DeviceSetting &setting) {
        if (!device || !device->automatable() || !device->matches(setting) || setting.automation.empty())
            return;
        audio::AutomationCurves curves;
        std::vector<audio::DeviceParam> params;
        for (const audio::AutomationLane &lane : setting.automation) {
            if (lane.points.empty())
                continue;
            if (params.empty())
                params = device->params();
            const auto param = std::find_if(params.begin(), params.end(),
                                            [&lane](const audio::DeviceParam &p) { return p.id == lane.param; });
            if (param == params.end() || param->readOnly)
                continue;
            audio::AutomationCurve curve;
            curve.param = lane.param;
            curve.curve = automationCurve(*param);
            curve.points = lane.points;
            curves.push_back(std::move(curve));
        }
        devices.emplace_back(device, std::move(curves));
    };
    if (instrument && !instrument->empty())
        add(chain.instrument(), *instrument);
    for (size_t j = 0; j < effects.size(); ++j)
        add(chain.effect(j), effects[j]);
    // The track's own: its volume on its fader's taper, its pan
    audio::AutomationCurves mixCurves;
    for (size_t k = 0; mixLanes && k < mixLanes->size(); ++k) {
        const audio::AutomationLane &lane = (*mixLanes)[k];
        if (lane.points.empty() || (lane.param != audio::kVolumeParam && lane.param != audio::kPanParam && !isSendParam(lane.param)))
            continue;
        audio::AutomationCurve curve;
        curve.param = lane.param;
        curve.curve = automationCurve(lane.param);
        curve.points = lane.points;
        mixCurves.push_back(std::move(curve));
    }
    chain.setAutomation(devices, std::move(mixCurves));
}

void AudioBridge::syncDevice(ProjectStore &store, const DevicePlace &place, audio::Device *device)
{
    const audio::DeviceSetting *now = store.project().device(place);
    plugin::Slot *slot = device && now && device->matches(*now) ? device->slot() : nullptr;
    if (!slot)
        return;
    plugin::ParamValues changes = slot->takeChanges();
    // An automated parameter's value is the automation's, not a change of the project's own
    changes.erase(std::remove_if(changes.begin(), changes.end(),
                                 [now](const auto &v) { return audio::automated(now->automation, v.first); }),
                  changes.end());
    const bool renamed = slot->state() == plugin::Slot::State::Ready && now->name != slot->name();
    // Only a change the project doesn't have yet is written (this runs every frame)
    bool differs = renamed;
    for (const auto &v : changes)
        differs = differs || now->value(v.first, std::nan("")) != v.second;
    if (!differs)
        return;
    audio::DeviceSetting *setting = store.editDevice(place);
    if (!setting)
        return;
    for (const auto &v : changes)
        setting->setValue(v.first, v.second);
    if (renamed)
        setting->name = slot->name();
}

void AudioBridge::setAudition(ObjectId track, ObjectId lane)
{
    const auto found = audition_.find(track);
    const ObjectId was = found == audition_.end() ? kNoId : found->second;
    if (was == lane)
        return;
    if (lane == kNoId)
        audition_.erase(track);
    else
        audition_[track] = lane;
    ++auditionVersion_;
}

ObjectId AudioBridge::audition(ObjectId track) const
{
    const auto found = audition_.find(track);
    return found == audition_.end() ? kNoId : found->second;
}

const std::vector<Clip> &AudioBridge::playedClips(const Track &t) const
{
    for (const TakeLane &lane : t.takeLanes) {
        if (lane.id != kNoId && lane.id == audition(t.id))
            return lane.clips;
    }
    return t.clips;
}

uint64_t AudioBridge::hashClips(const Project &project, const std::vector<Clip> &played) const
{
    uint64_t h = 1469598103934665603ull;
    for (const Clip &c : played) {
        mix(h, &c.start, sizeof(c.start));
        mix(h, &c.length, sizeof(c.length));
        mix(h, &c.offset, sizeof(c.offset));
        mix(h, &c.gain, sizeof(c.gain));
        mix(h, &c.fadeIn, sizeof(c.fadeIn));
        mix(h, &c.fadeOut, sizeof(c.fadeOut));
        mix(h, c.audioFile.data(), c.audioFile.size());
        mix(h, &c.take, sizeof(c.take));
        mix(h, &c.id, sizeof(c.id));
        const audio::Warp &w = c.warp;
        const uint8_t settings[4] = {static_cast<uint8_t>(w.on), static_cast<uint8_t>(w.mode),
                                     static_cast<uint8_t>(w.preserve), static_cast<uint8_t>(w.loop)};
        mix(h, settings, sizeof(settings));
        mix(h, &w.bpm, sizeof(w.bpm));
        mix(h, &w.pitch, sizeof(w.pitch));
        mix(h, &w.envelope, sizeof(w.envelope));
        mix(h, &w.grain, sizeof(w.grain));
        if (!w.markers.empty())
            mix(h, w.markers.data(), w.markers.size() * sizeof(audio::WarpMarker));
    }
    // A tempo change moves clips in time; a file finishing loading adds it
    const uint64_t tempo = tempoHash(project.timing);
    mix(h, &tempo, sizeof(tempo));
    const uint64_t version = pool_.version();
    mix(h, &version, sizeof(version));
    return h;
}

std::unique_ptr<audio::ClipSequence> AudioBridge::clipSequenceFor(const Project &project, const std::vector<Clip> &played)
{
    const audio::TempoMap &timing = project.timing;
    auto sequence = std::make_unique<audio::ClipSequence>();
    std::vector<std::pair<double, bool>> sections; // each region's start, and whether it is a comp section
    for (const Clip &c : played) {
        if (!c.hasAudio())
            continue;
        auto data = pool_.get(c.audioFile); // starts loading if it isn't yet
        if (!data)
            continue;
        audio::ClipRegion r;
        r.start = c.start;
        r.end = c.start + c.length;
        r.data = data;
        r.offset = c.offset;
        r.gain = c.gain;
        r.fadeIn = c.fadeIn;
        r.fadeOut = c.fadeOut;
        r.key = c.id;
        if (c.warp.on) {
            r.warp = c.warp;
            r.contentStart = clipContentStart(c);
            // Beats slices at the transients (or the grid); Complex starts its phases again at them
            if (c.warp.mode == audio::WarpMode::Beats || c.warp.mode == audio::WarpMode::Complex) {
                std::shared_ptr<const audio::AudioAnalysis> analysis = pool_.analysis(c.audioFile);
                if (c.warp.mode == audio::WarpMode::Beats)
                    r.slices = std::make_shared<const std::vector<audio::WarpSlice>>(
                        warpSlices(c, analysis.get(), data->sampleRate, data->frames));
                if (c.warp.mode == audio::WarpMode::Complex && analysis) {
                    std::vector<double> resets;
                    for (const audio::Transient &t : analysis->transients) {
                        if (t.strength >= 0.3f)
                            resets.push_back(static_cast<double>(t.frame));
                    }
                    r.resets = std::make_shared<const std::vector<double>>(std::move(resets));
                }
            }
        }
        sequence->regions.push_back(std::move(r));
        sections.push_back({c.start, c.take != kNoId});
    }
    sequence->sort();
    std::sort(sections.begin(), sections.end());
    // Where two comp sections meet, each reaches kCompFade / 2 into the other (where its sound goes
    // on) and they crossfade there, so a comp edit never clicks. The model keeps them butted.
    for (size_t i = 0; i + 1 < sequence->regions.size() && i + 1 < sections.size(); ++i) {
        audio::ClipRegion &a = sequence->regions[i];
        audio::ClipRegion &b = sequence->regions[i + 1];
        if (!sections[i].second || !sections[i + 1].second || std::fabs(a.end - b.start) > 1e-6)
            continue;
        const double edge = timing.secondsAt(a.end);
        // a's sound after the edge, and b's before it
        const double aAt = a.warp.on ? audio::warpSeconds(a.warp, a.contentStart + a.end - a.start)
                                     : a.offset + edge - timing.secondsAt(a.start);
        const double bAt = b.warp.on ? audio::warpSeconds(b.warp, b.contentStart) : b.offset;
        const double half = std::min({kCompFade / 2.0, std::max(0.0, a.data->seconds() - aAt), bAt});
        if (half <= 0.0)
            continue;
        a.end = timing.beatAt(edge + half);
        const double bStart = timing.beatAt(edge - half);
        b.offset -= half;
        b.contentStart -= b.start - bStart;
        b.start = bStart;
    }
    sequence->crossfade(timing);
    return sequence;
}

}
