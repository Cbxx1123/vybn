#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioPool.h"
#include "Library.h"
#include "ProjectStore.h"
#include "audio/AudioSystem.h"
#include "audio/ClipPlayer.h"
#include "audio/Midi.h"
#include "audio/NotePlayer.h"
#include "audio/PluginInstrument.h"
#include "audio/Track.h"

namespace daw {

// Keeps the audio engine in step with the project. Each track of the project has a TrackProcessor
// in the AudioSystem's graph (made by sync, taken out by removeTrack or dropAll), and the players
// behind it: an instrument track's NotePlayer, with a library instrument loaded in the background
// and handed to its sampler, and an audio track's AudioClipPlayer. sync runs once a frame: it copies
// each track's mix settings to its processor, sends notes and clips when they change, applies the
// devices (the master's and the tracks') and writes the plugins' own changes back into the project,
// and sets each track's delay compensation. The per-track state lives in one list, so it can't get
// out of step when a track is removed or the project is replaced.
class AudioBridge {
public:
    AudioBridge(audio::AudioSystem &audio, audio::MidiHub &midi, plugin::PluginManager *plugins,
                InstrumentLibrary &library, AudioPool &pool);

    AudioBridge(const AudioBridge &) = delete;
    AudioBridge &operator=(const AudioBridge &) = delete;

    struct SyncOptions {
        bool listen = true; // MIDI listening allowed (false while exporting)
    };
    // Once a frame: adds a processor for each new track and brings every track in line with the
    // project (see the class comment)
    void sync(ProjectStore &store, const SyncOptions &options);
    // Takes every track's processor out of the audio graph and forgets the per-track state; sync
    // adds them again for the project's tracks
    void dropAll();
    // Takes a deleted track's processor out of the audio graph and forgets its state
    void removeTrack(int index);
    // Before saving: each plugin's state now, into the project
    void captureStates(ProjectStore &store);
    // Tracks that asked for the library instrument `name` ask again (it was just installed)
    void reloadInstrument(const std::string &name);
    // Whether a track's library instrument has failed to load, or has loaded and been handed to
    // its sampler (both false while it is still loading, or for a track that doesn't ask for one)
    bool instrumentFailed(size_t track) const;
    bool instrumentReady(size_t track, const std::string &name) const;
    // What a track's library instrument is doing: loading, its memory, or why it is silent
    std::string instrumentStatus(const Project &project, int track) const;
    // The audio side's chain (the master's, or a track's): nullptr until sync has made it
    audio::DeviceChain *chainOf(bool master, int track);
    // The device at a place, if the audio side has made the one the project has there now
    audio::Device *deviceAt(const Project &project, const DevicePlace &place);
    // Every plugin in use: effects and instruments
    std::vector<plugin::Slot *> pluginSlots();
    // Frames a track's plugins (its instrument and the effects not bypassed) delay its sound
    uint32_t trackLatency(size_t track) const;
    // "Plugin latency 5.3 ms, held 2.1 ms to line up", or empty when no plugin delays anything
    std::string latencyText(int track) const;
    // The track's processor in the graph (nullptr until sync has added it, or out of range)
    audio::TrackProcessor *processor(int track) const;
    // The note player of an instrument track (nullptr for audio tracks, or out of range)
    audio::NotePlayer *player(int track) const;
    // Releases a key on every instrument track's player (a key held for audition let go)
    void releaseKey(int key);
    // Each track's peak this frame, for the mixer's meters
    const std::vector<float> &levels() const { return levels_; }

private:
    // Everything the bridge keeps for one track. Instrument tracks have a player, audio tracks a
    // clip player; the instrument fields are for instrument tracks with a library instrument.
    struct TrackAudio {
        audio::TrackProcessor *processor = nullptr; // owned by the AudioSystem
        audio::NotePlayer *player = nullptr;        // instrument tracks
        audio::AudioClipPlayer *clips = nullptr;    // audio tracks
        uint64_t noteHash = 0;                      // the notes last sent
        uint64_t clipHash = 0;                      // the clips last sent
        // The track's revision (and the pool's version and the tempo, which clips depend on) when its
        // notes or clips were last checked: sync only hashes again when one of them has moved
        uint64_t notesRevision = 0;
        uint64_t clipsRevision = 0;
        uint64_t clipsPoolVersion = ~0ull;
        double clipsTempo = 0.0;
        std::string requested = std::string(1, '\0'); // the instrument asked for (never a name at first)
        audio::SamplerDevice *sampler = nullptr;    // the sampler the request went to
        std::shared_ptr<const audio::SampleSet> pendingSet; // loaded, waiting for the sampler
        std::string error;                          // why loading failed
        uint32_t latency = 0;                       // its plugins' latency at the last sync
    };

    // The plugin's own changes (and name) into the project, when they really differ from it
    void syncDevice(ProjectStore &store, const DevicePlace &place, audio::Device *device);
    uint64_t hashClips(const Project &project, const Track &t) const;
    std::unique_ptr<audio::ClipSequence> clipSequenceFor(const Project &project, const Track &t);

    audio::AudioSystem &audio_;
    audio::MidiHub &midi_;
    plugin::PluginManager *plugins_;
    InstrumentLibrary &library_;
    AudioPool &pool_;
    std::shared_ptr<audio::SoloGroup> solo_ = std::make_shared<audio::SoloGroup>();
    std::vector<TrackAudio> tracks_;
    std::vector<float> levels_; // kept as its own vector: MixerView holds a reference to it
};

}
