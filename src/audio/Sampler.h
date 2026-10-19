#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "Instrument.h"
#include "SampleSet.h"
#include "env/SpscQueue.h"

namespace audio {

// The sampler engine: plays a SampleSet's zones, pitch-shifted to each key (linear
// interpolation), with velocity picking the layer (every zone covering the key and velocity
// sounds, up to kMaxLayers), loops, a release fade after note off, and voice stealing when every
// voice is busy. A new note on a key that is still sounding cuts the old one short, as a
// plucked string does.
//
// The sample set is swapped in from the main thread with setSampleSet. The audio thread picks
// it up at the start of a block, stopping the old set's voices, and hands the old set back;
// collect() then lets go of it, so sample memory is never freed on the audio thread.
class Sampler : public Instrument {
public:
    static constexpr int kVoices = 48;
    static constexpr int kMaxLayers = 4; // zones started by one note at most

    explicit Sampler(std::string name = "Sampler");
    ~Sampler() override;

    // Main thread. nullptr silences the sampler. Returns false, changing nothing, when the audio
    // thread hasn't taken the previous changes yet (no device running): try again later.
    bool setSampleSet(std::shared_ptr<const SampleSet> set);
    // The set most recently given to setSampleSet (it may not be playing yet)
    const std::shared_ptr<const SampleSet> &sampleSet() const { return latest_; }
    void collect() override;

    AudioParam gain{1.0f, 0.0f, 2.0f}; // linear, applied to every voice
    // Voices sounding now (audio thread writes it; for display)
    int activeVoices() const { return active_.load(std::memory_order_relaxed); }

    void noteOn(int key, float velocity) override;
    void noteOff(int key) override;
    void allNotesOff() override;
    void beginBlock() override;
    // The mod wheel (1) adds vibrato, volume (7) and expression (11) scale the level, and the
    // pitch bend bends every voice up to two semitones
    void controlChange(int controller, float value) override;
    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

private:
    struct Voice {
        const SampleZone *zone = nullptr;
        int key = -1;
        double position = 0.0; // in the zone's frames
        double step = 1.0;     // zone frames per output frame
        float level = 0.0f;    // velocity times the zone's gain
        float envelope = 1.0f; // falls during the release
        float decay = 1.0f;    // per-frame envelope factor while releasing
        bool releasing = false;
        uint64_t started = 0;  // for stealing the oldest
    };

    void release(Voice &voice, float seconds);
    Voice &freeVoice();

    // Main thread
    std::vector<std::shared_ptr<const SampleSet>> holds_; // every set the audio thread may use
    std::shared_ptr<const SampleSet> latest_;

    // Main -> audio and back
    env::SpscQueue<const SampleSet *, 16> incoming_;
    env::SpscQueue<const SampleSet *, 16> retired_;

    // Audio thread
    const SampleSet *current_ = nullptr;
    std::array<Voice, kVoices> voices_{};
    double sampleRate_ = 48000.0;
    float modWheel_ = 0.0f, volume_ = 100.0f / 127.0f, expression_ = 1.0f, bend_ = 0.5f;
    float level_ = 1.0f;      // the controllers' gain, glided across each block
    double vibrato_ = 0.0;    // the vibrato's phase, 0-1
    uint64_t notes_ = 0;
    std::atomic<int> active_{0};
};

}
