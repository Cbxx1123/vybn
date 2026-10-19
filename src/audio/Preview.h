#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "AudioFile.h"
#include "AudioProcessor.h"
#include "env/SpscQueue.h"

namespace audio {

// Plays one sound file from its start, at once and whatever the transport does: for listening to
// samples in the browser. The sound is swapped in through a queue and handed back for collect()
// to let go of, so sample memory is never freed on the audio thread. A new sound or stop() fades
// the old one out in a few milliseconds, against clicks.
class SoundPreview : public AudioProcessor {
public:
    SoundPreview();

    // Main thread. Returns false when the audio thread hasn't taken the last change yet.
    bool play(std::shared_ptr<const AudioData> data);
    bool stop() { return play(nullptr); }
    void collect();

    AudioParam gain{0.8f, 0.0f, 1.5f};
    // Whether something is sounding, and how far into it (seconds); audio thread writes them
    bool playing() const { return playing_.load(std::memory_order_relaxed); }
    double position() const { return position_.load(std::memory_order_relaxed); }

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

private:
    // Main thread
    std::vector<std::shared_ptr<const AudioData>> holds_;
    // Main -> audio and back
    env::SpscQueue<const AudioData *, 16> incoming_;
    env::SpscQueue<const AudioData *, 16> retired_;
    // Audio thread
    const AudioData *current_ = nullptr;
    const AudioData *fading_ = nullptr; // the previous sound, fading out
    double cursor_ = 0.0;               // frames into current_
    double fadeCursor_ = 0.0;
    float fade_ = 0.0f;                 // fading_'s level, falling to 0
    double sampleRate_ = 48000.0;
    std::atomic<bool> playing_{false};
    std::atomic<double> position_{0.0};
};

}
