#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

// What one window's last frame cost, for Settings > Metrics, VIBIN_FRAME_STATS and the render
// self-test. Times are milliseconds of CPU on the main thread.
struct FrameStats {
    // Context::update: layout, input, keys and posted actions (layoutMs is its share and
    // buildFrame's laying out)
    double updateMs = 0.0;
    double layoutMs = 0.0;
    // Context::buildFrame: laying out, drawing the tree into the DrawList and the fingerprint
    double buildMs = 0.0;
    // Context::submit: the UIRenderer turning the list into instances and recording draws (0 when
    // the frame wasn't presented)
    double submitMs = 0.0;
    bool presented = false;

    // What the last buildFrame drew
    uint32_t surfaces = 0;
    uint32_t texts = 0;
    uint32_t glyphs = 0; // filled by submit, when the text is laid out
    uint32_t images = 0;
    uint32_t batches = 0;

    // What the last submit recorded
    uint32_t draws = 0;          // draw calls
    uint32_t pipelineBinds = 0;  // pipeline binds
    size_t uploadBytes = 0;      // instance data written this frame
};

}
