#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "AudioFile.h"
#include "AudioProcessor.h"
#include "env/SpscQueue.h"

namespace audio {

// A piece of a sound file on the timeline
struct ClipRegion {
    double start = 0.0; // beats on the timeline
    double end = 0.0;
    std::shared_ptr<const AudioData> data;
    double offset = 0.0; // seconds into the file at `start` (trimming the front moves it)
    float gain = 1.0f;
    double fadeIn = 0.0;  // seconds, equal-power
    double fadeOut = 0.0;
};

// Everything an audio track plays, sorted by start
struct ClipSequence {
    std::vector<ClipRegion> regions;

    void sort();
    // Where regions overlap, the earlier fades out and the later fades in across the overlap,
    // unless their own fades are longer: a crossfade. `timing` turns beats into seconds.
    void crossfade(const TempoMap &timing);
};

// An audio track's source: plays its clips in time with the transport (at the file's own speed,
// resampled to the device's rate; tempo changes don't stretch them) and, with `monitor` set,
// passes the live input through so a player hears themselves. The sequence is swapped from the
// main thread through a queue, as NotePlayer does; old ones (and the audio they hold) come back
// for collect() to free, so no sample memory is freed on the audio thread.
class AudioClipPlayer : public AudioProcessor {
public:
    AudioClipPlayer();
    ~AudioClipPlayer() override;

    // Main thread. Returns false when the audio thread hasn't taken the previous ones yet.
    bool setSequence(std::unique_ptr<ClipSequence> sequence);
    void collect();

    // The input to monitor: its first channel, and whether the next one is its right side
    std::atomic<bool> monitor{false};
    std::atomic<int> inputChannel{0};
    std::atomic<bool> inputStereo{false};

    void process(AudioBlock &block) override;

private:
    env::SpscQueue<ClipSequence *, 16> incoming_;
    env::SpscQueue<ClipSequence *, 16> retired_;
    ClipSequence *current_ = nullptr; // audio thread
};

}
