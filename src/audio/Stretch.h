#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/AudioProcessor.h"
#include "audio/Fft.h"
#include "audio/Warp.h"

// The stretch voices that play warped audio clips (docs/design/warp-plan.md). A voice is made on the
// main thread with every buffer it will need and then only renders on the audio thread.

namespace audio {

struct ClipRegion;

// The most frames a voice renders in one call (AudioSystem::kMaxBlock)
constexpr uint32_t kMaxStretchBlock = 4096;

// Where a warped region reads its file for any frame of the block (from the block's start; negative
// before it): the transport's beat, the clip's content beat, the warp map, the file's frames. The map
// is known ahead, so a voice can read the frames it will need before it plays them.
struct WarpPath {
    const TransportState *transport = nullptr;
    const ClipRegion *region = nullptr;

    double content(double frame) const;
    double at(double frame) const; // file frames
};

// A voice: one warped region's stretcher. Bound to the region's key while it plays (so it keeps its
// state across a new sequence with the same clip), reset when the transport jumps. Where the region
// plays at its file's own speed and pitch the file is read directly, sample for sample as an unwarped
// clip; changing between that and stretching crossfades over kSwitchSeconds.
class StretchVoice {
public:
    static constexpr double kSwitchSeconds = 0.005;

    explicit StretchVoice(WarpMode mode);
    virtual ~StretchVoice() = default;

    WarpMode mode() const { return mode_; }

    // Audio thread: the region it is playing (0: free), and whether it played this block
    uint64_t key = 0;
    bool used = false;
    void unbind();

    // Frames [from, to) of the block, written (not added) into left and right
    void render(const ClipRegion &region, const WarpPath &path, const TransportState &t, uint32_t from, uint32_t to,
                float *left, float *right);
    // Whether the last render played the file directly
    bool direct() const { return !stretching_; }

protected:
    // The mode's own rendering of [from, to); `fresh` when it must start cold at `from` (a jump, the
    // first block, or coming back from direct reading)
    virtual void stretch(const ClipRegion &region, const WarpPath &path, const TransportState &t, uint32_t from,
                         uint32_t to, float *left, float *right, bool fresh) = 0;

private:
    WarpMode mode_;
    bool stretching_ = false; // what it plays once a switch has faded
    uint32_t fade_ = 0;       // frames of the switch left
    uint32_t fadeLength_ = 0;
    uint64_t next_ = 0;       // the transport sample the stretcher would continue at
    bool running_ = false;    // the stretcher rendered the last frames
    bool started_ = false;    // rendered since it was bound
    std::vector<float> otherLeft_, otherRight_;
};

// A stretch voice for a mode (not Repitch, which needs none)
std::unique_ptr<StretchVoice> makeStretchVoice(WarpMode mode);

// Reads the file directly at the path's positions (linear, as unwarped clips are)
void renderDirect(const ClipRegion &region, const WarpPath &path, uint32_t from, uint32_t to, float *left, float *right);
// Repitch: the file read at the speed the path gives (Hermite), pitch going with it
void renderRepitch(const ClipRegion &region, const WarpPath &path, uint32_t from, uint32_t to, float *left, float *right);

// The step a voice reads its file at for a region: the transposition times the file's rate over the device's
double stretchStep(const ClipRegion &region, double deviceRate);

}
