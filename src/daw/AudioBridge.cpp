#include "AudioBridge.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Format.h"
#include "audio/DeviceChain.h"
#include "audio/PluginInstrument.h"
#include "plugin/Slot.h"

namespace daw {

namespace {

// FNV-1a over a track's clips and notes, to notice edits without tracking every one
uint64_t hashNotes(const Track &t)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *data, size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const Clip &c : t.clips) {
        mix(&c.start, sizeof(c.start));
        mix(&c.length, sizeof(c.length));
        for (const Note &n : c.notes) {
            mix(&n.start, sizeof(n.start));
            mix(&n.length, sizeof(n.length));
            mix(&n.key, sizeof(n.key));
            mix(&n.velocity, sizeof(n.velocity));
            mix(&n.muted, sizeof(n.muted));
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

// A track's notes on the timeline, cut at the end of their clips
std::unique_ptr<audio::NoteSequence> sequenceFor(const Track &t)
{
    auto sequence = std::make_unique<audio::NoteSequence>();
    for (const Clip &c : t.clips) {
        for (const Note &n : c.notes) {
            if (n.start >= c.length || n.length <= 0.0 || n.muted)
                continue;
            const double start = c.start + n.start;
            const double end = std::min(start + n.length, c.start + c.length);
            sequence->notes.push_back({start, end, n.key, n.velocity});
        }
    }
    // Controller lanes: each controller's points from every clip on the timeline, in order;
    // a clip's curve ends where it does, holding its last value there
    for (const Clip &c : t.clips) {
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

void AudioBridge::sync(ProjectStore &store, const SyncOptions &options)
{
    const Project &project = store.project();
    while (tracks_.size() < project.tracks.size()) {
        const Track &t = project.tracks[tracks_.size()];
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
        } else {
            auto audioClips = std::make_unique<audio::AudioClipPlayer>();
            fresh.clips = audioClips.get();
            source = std::move(audioClips);
        }
        fresh.processor = &audio_.add<audio::TrackProcessor>(t.name, solo_, std::move(source));
        fresh.processor->chain.setPlugins(plugins_);
        if (chainInstrument)
            chainInstrument->setChain(&fresh.processor->chain);
        tracks_.push_back(fresh);
    }
    levels_.resize(tracks_.size());
    // The master effects
    audio::DeviceChain &master = audio_.masterEffects();
    master.apply(project.masterEffects);
    master.collect();
    for (size_t j = 0; j < project.masterEffects.size(); ++j)
        syncDevice(store, DevicePlace::effect(true, 0, static_cast<int>(j)), master.effect(j));
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
        p.setSolo(t.solo);
        // Its devices: the instrument (instrument tracks) and the effects, and the plugins' own
        // changes back into the project
        static const audio::DeviceSetting kNone;
        p.chain.apply(t.kind == TrackKind::Instrument ? t.instrument : kNone, t.effects);
        p.chain.collect();
        if (t.kind == TrackKind::Instrument && t.hasInstrument())
            syncDevice(store, DevicePlace::instrumentOf(static_cast<int>(i)), p.chain.instrument());
        for (size_t j = 0; j < t.effects.size(); ++j)
            syncDevice(store, DevicePlace::effect(false, static_cast<int>(i), static_cast<int>(j)), p.chain.effect(j));
        levels_[i] = std::max(p.takePeak(0), p.takePeak(1));

        if (audio::AudioClipPlayer *clips = state.clips) {
            // The clips are looked at again only when the track, the pool or the tempo moved. A clip
            // sequence not sent yet (the queue is full) is sent again next frame, as before.
            const uint64_t trackRevision = store.trackRevision(static_cast<int>(i));
            const uint64_t poolVersion = pool_.version();
            if (trackRevision != state.clipsRevision || poolVersion != state.clipsPoolVersion ||
                project.tempo != state.clipsTempo) {
                const uint64_t hash = hashClips(project, t);
                if (hash == state.clipHash || clips->setSequence(clipSequenceFor(project, t))) {
                    state.clipHash = hash;
                    state.clipsRevision = trackRevision;
                    state.clipsPoolVersion = poolVersion;
                    state.clipsTempo = project.tempo;
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
        if (trackRevision != state.notesRevision) {
            const uint64_t hash = hashNotes(t);
            if (hash == state.noteHash || player->setSequence(sequenceFor(t))) {
                state.noteHash = hash;
                state.notesRevision = trackRevision;
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
                library_.load(name, [this, i, name](std::shared_ptr<const audio::SampleSet> set, const std::string &error) {
                    if (i >= tracks_.size() || tracks_[i].requested != name)
                        return; // the track has moved on, or the project was replaced
                    if (set)
                        tracks_[i].pendingSet = std::move(set);
                    else
                        tracks_[i].error = error;
                });
            }
        }
        if (sampler && state.pendingSet && sampler->sampler().setSampleSet(state.pendingSet))
            state.pendingSet = nullptr;
        player->collect();
    }

    // Delay compensation: every track held back to line up with the one its plugins delay most
    for (size_t i = 0; i < tracks_.size(); ++i)
        tracks_[i].latency = trackLatency(i);
    std::vector<uint32_t> latencies;
    for (const TrackAudio &t : tracks_)
        latencies.push_back(t.latency);
    const std::vector<uint32_t> delays = audio::compensations(latencies);
    for (size_t i = 0; i < tracks_.size(); ++i)
        tracks_[i].processor->compensation = delays[i];
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
    // tracks find their index gone (or another name there) and are dropped.
    for (const TrackAudio &t : tracks_)
        audio_.remove(*t.processor);
    tracks_.clear();
    levels_.clear();
}

void AudioBridge::removeTrack(int index)
{
    const size_t i = static_cast<size_t>(index);
    if (i >= tracks_.size())
        return;
    audio_.remove(*tracks_[i].processor);
    tracks_.erase(tracks_.begin() + index);
    if (i < levels_.size())
        levels_.erase(levels_.begin() + index);
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
    for (size_t i = 0; i < tracks_.size() && i < project.tracks.size(); ++i) {
        const Track &t = project.tracks[i];
        if (t.kind == TrackKind::Instrument && t.hasInstrument())
            capture(tracks_[i].processor->chain.instrument(), DevicePlace::instrumentOf(static_cast<int>(i)));
        for (size_t j = 0; j < t.effects.size(); ++j)
            capture(tracks_[i].processor->chain.effect(j), DevicePlace::effect(false, static_cast<int>(i), static_cast<int>(j)));
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
    const uint32_t mine = tracks_[static_cast<size_t>(track)].latency;
    uint32_t most = 0;
    for (const TrackAudio &t : tracks_)
        most = std::max(most, t.latency);
    if (most == 0)
        return {};
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    if (mine == most)
        return format("Plugin latency %.1f ms", mine * 1000.0 / rate);
    return format("Plugin latency %.1f ms", mine * 1000.0 / rate) + format(", held %.1f ms to line up", (most - mine) * 1000.0 / rate);
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

void AudioBridge::syncDevice(ProjectStore &store, const DevicePlace &place, audio::Device *device)
{
    const audio::DeviceSetting *now = store.project().device(place);
    plugin::Slot *slot = device && now && device->matches(*now) ? device->slot() : nullptr;
    if (!slot)
        return;
    const plugin::ParamValues changes = slot->takeChanges();
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

uint64_t AudioBridge::hashClips(const Project &project, const Track &t) const
{
    uint64_t h = 1469598103934665603ull;
    for (const Clip &c : t.clips) {
        mix(h, &c.start, sizeof(c.start));
        mix(h, &c.length, sizeof(c.length));
        mix(h, &c.offset, sizeof(c.offset));
        mix(h, &c.gain, sizeof(c.gain));
        mix(h, &c.fadeIn, sizeof(c.fadeIn));
        mix(h, &c.fadeOut, sizeof(c.fadeOut));
        mix(h, c.audioFile.data(), c.audioFile.size());
    }
    // A tempo change moves clips in time; a file finishing loading adds it
    mix(h, &project.tempo, sizeof(project.tempo));
    const uint64_t version = pool_.version();
    mix(h, &version, sizeof(version));
    return h;
}

std::unique_ptr<audio::ClipSequence> AudioBridge::clipSequenceFor(const Project &project, const Track &t)
{
    auto sequence = std::make_unique<audio::ClipSequence>();
    for (const Clip &c : t.clips) {
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
        sequence->regions.push_back(std::move(r));
    }
    sequence->sort();
    sequence->crossfade(project.tempo);
    return sequence;
}

}
