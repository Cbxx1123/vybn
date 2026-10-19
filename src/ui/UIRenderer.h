#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "DrawList.h"
#include "Font.h"
#include "FrameStats.h"
#include "engine/Renderer.h"

namespace ui {

// Draws a DrawList's prims in order with one pipeline (shaders/prim.vert and prim.frag) for every
// kind: surfaces, glyphs and images. Each maximal run of surfaces and glyphs is one instanced draw,
// and each image is a draw of its own. Set 0 of the pipeline is the font atlas, set 1 the image
// being drawn (a blank one when none is). Each prim is clipped in the shader, so there are no
// scissors. Its shaders, pipeline and the atlas's texture are shared by every UIRenderer on the
// same device and font, so another window costs only its buffers. Must be destroyed before the
// engine::Renderer; `font` must outlive it.
class UIRenderer {
public:
    UIRenderer(engine::Renderer &renderer, const Font &font);

    // Call between Renderer::beginFrame and Renderer::endFrame
    void render(const DrawList &list);

    // What the last render recorded: its time (submitMs), draw calls, pipeline binds and bytes of
    // instance data written; the other fields are left at 0 (Context fills them)
    const FrameStats &stats() const { return stats_; }

private:
    engine::Renderer &renderer_;
    const Font &font_;

    // The pipelines and the atlas, shared between windows
    struct Shared;
    std::shared_ptr<Shared> shared_;

    // One per frame in flight: the frame's prims, every kind
    std::vector<engine::Buffer> primBuffers_;

    // The descriptor set showing `image` for frame slot `frame`, its texture brought up to date
    const engine::DescriptorSet *liveImage(const ImageData &image, uint32_t frame);

    // Live images by ImageData::id: a texture and descriptor set per frame in flight, each
    // rewritten when its frame comes round and the image's version has moved; let go of a few
    // frames after the image was last drawn
    struct LiveImage {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<engine::LiveTexture> textures;
        std::unique_ptr<engine::DescriptorPool> pool;
        std::vector<engine::DescriptorSet> sets;
        std::vector<uint64_t> versions;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<uint64_t, LiveImage> images_;
    uint64_t frames_ = 0;
    FrameStats stats_;
    bool imagesFailed_ = false; // the device can't show them: images are skipped
};

}
