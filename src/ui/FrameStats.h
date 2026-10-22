#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

// What one window's last frame cost, for Settings > Metrics, VYBN_FRAME_STATS and the render
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
    uint32_t glyphs = 0; // text is laid out as it is added, one prim per glyph
    uint32_t images = 0;
    uint32_t lines = 0;
    uint32_t plots = 0;
    size_t plotValues = 0; // floats of plot data

    // Draw caches (DrawCache) in the last buildFrame: sections appended again, sections painted,
    // and the prims the appended ones brought
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t cachedPrims = 0;
    // Whether the last update skipped its layout (the tree, window and theme were as the last
    // buildFrame laid them out)
    bool layoutSkipped = false;
    uint32_t batches = 0; // always 0 since the unified pipeline (no batches); kept for old readers

    // What the last submit recorded
    uint32_t draws = 0;          // draw calls
    uint32_t pipelineBinds = 0;  // pipeline binds
    size_t uploadBytes = 0;      // instance data written this frame
};

}
