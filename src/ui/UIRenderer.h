#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "DrawList.h"
#include "Font.h"
#include "engine/Renderer.h"

namespace ui {

// Draws a DrawList in order, one instanced call per batch: each Surface becomes one quad shaded
// by shaders/ui.frag, and each glyph of a Text one quad sampling the font atlas in
// shaders/text.frag. Its shaders, pipelines and the font atlas's texture are shared by every
// UIRenderer on the same device and font, so another window costs only its buffers. Must be
// destroyed before the engine::Renderer; `font` must outlive it.
class UIRenderer {
public:
    UIRenderer(engine::Renderer &renderer, const Font &font);

    // Call between Renderer::beginFrame and Renderer::endFrame
    void render(const DrawList &list);

private:
    engine::Renderer &renderer_;
    const Font &font_;

    // The pipelines and the atlas, shared between windows
    struct Shared;
    std::shared_ptr<Shared> shared_;

    // One per frame in flight
    std::vector<engine::Buffer> surfaceBuffers_;
    std::vector<engine::Buffer> glyphBuffers_;

    std::vector<GlyphQuad> quads_; // scratch, reused each frame
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
    std::vector<engine::Buffer> imageBuffers_;
    uint64_t frames_ = 0;
    bool imagesFailed_ = false; // the device can't show them: images are skipped
};

}
