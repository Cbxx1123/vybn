#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/AudioProcessor.h"
#include "audio/Stretch.h"
#include "audio/Warp.h"
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

    // Warping (docs/design/warp-plan.md): with warp.on the region follows the bars, content beat
    // `contentStart` of its warp map playing at `start`, and `offset` isn't used
    Warp warp;
    double contentStart = 0.0;
    uint64_t key = 0; // the clip's id: a stretch voice stays with the clip across sequences
    std::shared_ptr<const std::vector<WarpSlice>> slices; // Beats: where it slices, in order
    std::shared_ptr<const std::vector<double>> resets;    // Complex: its transients (file frames), in order
};

// Everything an audio track plays, sorted by start
struct ClipSequence {
    std::vector<ClipRegion> regions;
    // Stretch voices the player didn't have yet, made with the sequence on the main thread
    // (AudioClipPlayer::setSequence) and taken by the audio thread into its own
    std::vector<std::unique_ptr<StretchVoice>> voices;

    void sort();
    // Where regions overlap, the earlier fades out and the later fades in across the overlap,
    // unless their own fades are longer: a crossfade. `timing` turns beats into seconds.
    void crossfade(const TempoMap &timing);
};

// An audio track's source: plays its clips in time with the transport (an unwarped clip at the
// file's own speed, resampled to the device's rate; a warped one following the bars, through a
// stretch voice) and, with `monitor` set, passes the live input through so a player hears
// themselves. The sequence is swapped from the main thread through a queue, as NotePlayer does; old
// ones (and the audio they hold) come back for collect() to free, so no sample memory is freed on
// the audio thread.
class AudioClipPlayer : public AudioProcessor {
public:
    AudioClipPlayer();
    ~AudioClipPlayer() override;

    // Main thread. Returns false when the audio thread hasn't taken the previous ones yet. Adds to the
    // sequence the stretch voices its warped regions need that the player hasn't been given yet.
    bool setSequence(std::unique_ptr<ClipSequence> sequence);
    void collect();

    // The input to monitor: its first channel, and whether the next one is its right side
    std::atomic<bool> monitor{false};
    std::atomic<int> inputChannel{0};
    std::atomic<bool> inputStereo{false};

    void process(AudioBlock &block) override;

    // The stretch voices it has (audio thread; tests read them while it is stopped)
    static constexpr int kVoicesPerMode = 4;
    int voiceCount() const { return voiceCount_; }
    const StretchVoice *voice(int i) const { return voices_[i]; }

private:
    static constexpr int kModes = 4;
    StretchVoice *bindVoice(const ClipRegion &region, uint64_t key);

    env::SpscQueue<ClipSequence *, 16> incoming_;
    env::SpscQueue<ClipSequence *, 16> retired_;
    ClipSequence *current_ = nullptr; // audio thread
    int granted_[kModes] = {};        // main thread: the voices of each mode sent so far
    StretchVoice *voices_[kModes * kVoicesPerMode] = {}; // audio thread: owned
    int voiceCount_ = 0;
    std::vector<float> scratchLeft_, scratchRight_; // a warped region's frames before its gain and fades
};

}
