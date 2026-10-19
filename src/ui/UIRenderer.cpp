#include "UIRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace ui {

namespace {

// Matches the instance inputs in shaders/ui.vert
struct Instance {
    float rect[4];  // x, y, w, h
    float shape[4]; // radius, distance, blur, elevation
    float base[4];  // rgb, opacity
    float light[4]; // rgb, intensity
    float dark[4];  // rgb, intensity
    float style[4]; // light direction x, y, curvature, form
};

// Matches the instance inputs in shaders/text.vert
struct GlyphInstance {
    float rect[4];  // x0, y0, x1, y1
    float uv[4];    // u0, v0, u1, v1
    float color[4]; // rgb, alpha
};

constexpr VkDeviceSize kInitialCapacity = 256 * sizeof(Instance);
constexpr VkDeviceSize kInitialGlyphCapacity = 1024 * sizeof(GlyphInstance);

engine::VertexLayout instanceLayout()
{
    engine::VertexLayout layout{
        sizeof(Instance),
        {
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, rect)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, shape)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, base)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, light)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, dark)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, style)},
        },
    };
    layout.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    return layout;
}

engine::VertexLayout glyphLayout()
{
    engine::VertexLayout layout{
        sizeof(GlyphInstance),
        {
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlyphInstance, rect)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlyphInstance, uv)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlyphInstance, color)},
        },
    };
    layout.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    return layout;
}

engine::PipelineOptions pipelineOptions(const engine::DescriptorSetLayout *descriptors = nullptr)
{
    engine::PipelineOptions options;
    options.alphaBlending = true;
    options.pushConstantSize = 2 * sizeof(float); // screen size
    options.descriptorSetLayout = descriptors;
    return options;
}

Instance toInstance(const Surface &s)
{
    return Instance{
        {s.rect.x, s.rect.y, s.rect.w, s.rect.h},
        {s.radius, s.distance, s.blur, static_cast<float>(s.elevation) + std::clamp(s.rim, 0.0f, 1.0f) * 0.45f},
        {s.base.r, s.base.g, s.base.b, s.opacity},
        {s.light.r, s.light.g, s.light.b, s.intensity},
        {s.dark.r, s.dark.g, s.dark.b, s.intensity},
        {s.lightDirection.x, s.lightDirection.y, s.curvature, s.form},
    };
}

// Rewrites this frame's buffer, replacing it with a bigger one if needed. The buffer was last
// read two frames ago, and beginFrame waited for that frame, so this is safe after beginFrame.
template <typename T>
void upload(engine::Renderer &renderer, engine::Buffer &buffer, const std::vector<T> &items)
{
    if (items.empty())
        return;
    const VkDeviceSize bytes = items.size() * sizeof(T);
    if (bytes > buffer.capacity())
        buffer = renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, bytes * 2);
    buffer.write(items.data(), bytes, static_cast<uint32_t>(items.size()));
}

using Clock = std::chrono::steady_clock;

// Milliseconds since `start`
double msSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}

struct UIRenderer::Shared {
    // What it was made for: pipelines work with any window on the same device whose swapchain
    // has the same format (compatible render passes)
    VkDevice device;
    VkFormat format;
    const Font *font;

    engine::Shader surfaceVertex;
    engine::Shader surfaceFragment;
    engine::Pipeline surfacePipeline;

    engine::Texture atlas;
    engine::DescriptorSetLayout atlasLayout;
    engine::DescriptorPool descriptorPool;
    engine::DescriptorSet atlasSet;
    engine::Shader textVertex;
    engine::Shader textFragment;
    engine::Pipeline textPipeline;
    engine::Shader imageFragment;
    engine::Pipeline imagePipeline;

    Shared(engine::Renderer &renderer, const Font &f)
        : device(renderer.device()),
          format(renderer.format()),
          font(&f),
          surfaceVertex(renderer.createShader("ui.vert.spv", engine::ShaderStage::Vertex)),
          surfaceFragment(renderer.createShader("ui.frag.spv", engine::ShaderStage::Fragment)),
          surfacePipeline(renderer.createPipeline(surfaceVertex, surfaceFragment, instanceLayout(), pipelineOptions())),
          atlas(renderer.createTexture(f.atlasWidth(), f.atlasHeight(), VK_FORMAT_R8_UNORM, f.atlas().data(), f.atlas().size())),
          atlasLayout(renderer.createDescriptorSetLayout({{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}})),
          descriptorPool(renderer.createDescriptorPool(1, 1)),
          atlasSet(descriptorPool.allocate(atlasLayout)),
          textVertex(renderer.createShader("text.vert.spv", engine::ShaderStage::Vertex)),
          textFragment(renderer.createShader("text.frag.spv", engine::ShaderStage::Fragment)),
          textPipeline(renderer.createPipeline(textVertex, textFragment, glyphLayout(), pipelineOptions(&atlasLayout))),
          imageFragment(renderer.createShader("image.frag.spv", engine::ShaderStage::Fragment)),
          imagePipeline(renderer.createPipeline(textVertex, imageFragment, glyphLayout(), pipelineOptions(&atlasLayout)))
    {
        atlasSet.writeTexture(0, atlas);
    }
};

UIRenderer::UIRenderer(engine::Renderer &renderer, const Font &font)
    : renderer_(renderer),
      font_(font)
{
    // The resources the live UIRenderers share (main thread only)
    static std::vector<std::weak_ptr<Shared>> sharedResources;
    for (auto it = sharedResources.begin(); it != sharedResources.end();) {
        std::shared_ptr<Shared> s = it->lock();
        if (!s) {
            it = sharedResources.erase(it);
            continue;
        }
        if (s->device == renderer.device() && s->format == renderer.format() && s->font == &font)
            shared_ = s;
        ++it;
    }
    if (!shared_) {
        shared_ = std::make_shared<Shared>(renderer, font);
        sharedResources.push_back(shared_);
    }

    surfaceBuffers_.reserve(engine::Renderer::kMaxFramesInFlight);
    glyphBuffers_.reserve(engine::Renderer::kMaxFramesInFlight);
    for (uint32_t i = 0; i < engine::Renderer::kMaxFramesInFlight; ++i) {
        surfaceBuffers_.push_back(renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kInitialCapacity));
        glyphBuffers_.push_back(renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kInitialGlyphCapacity));
        imageBuffers_.push_back(renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 16 * sizeof(GlyphInstance)));
    }
}

void UIRenderer::render(const DrawList &list)
{
    const Clock::time_point start = Clock::now();
    stats_ = FrameStats{};
    ++frames_;
    // Images not drawn for a few frames go (no frame in flight still reads them)
    for (auto it = images_.begin(); it != images_.end();)
        it = it->second.lastFrame + engine::Renderer::kMaxFramesInFlight + 2 < frames_ ? images_.erase(it) : std::next(it);
    if (list.empty()) {
        stats_.submitMs = msSince(start);
        return;
    }

    // Flatten every batch into its instance array, remembering where each batch's instances start
    struct Range {
        DrawList::Kind kind;
        uint32_t first;
        uint32_t count;
        const DrawList::Batch *batch;
    };
    std::vector<Range> ranges;
    ranges.reserve(list.batches.size());
    std::vector<Instance> instances;
    instances.reserve(list.surfaces.size());
    std::vector<GlyphInstance> glyphs;

    size_t nextSurface = 0;
    size_t nextText = 0;
    size_t nextImage = 0;
    std::vector<GlyphInstance> imageQuads;
    std::vector<const engine::DescriptorSet *> imageSets;
    const uint32_t frame = renderer_.frameIndex();
    for (const DrawList::Batch &batch : list.batches) {
        if (batch.kind == DrawList::Kind::Image) {
            const uint32_t first = static_cast<uint32_t>(imageQuads.size());
            for (uint32_t i = 0; i < batch.count; ++i) {
                const ImageDraw &d = list.images[nextImage++];
                const engine::DescriptorSet *set = imagesFailed_ ? nullptr : liveImage(*d.image, frame);
                if (!set)
                    continue;
                imageQuads.push_back({{d.rect.x, d.rect.y, d.rect.x + d.rect.w, d.rect.y + d.rect.h}, {0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, d.opacity}});
                imageSets.push_back(set);
            }
            ranges.push_back({batch.kind, first, static_cast<uint32_t>(imageQuads.size()) - first, &batch});
            continue;
        }
        if (batch.kind == DrawList::Kind::Surface) {
            const uint32_t first = static_cast<uint32_t>(instances.size());
            for (uint32_t i = 0; i < batch.count; ++i)
                instances.push_back(toInstance(list.surfaces[nextSurface++]));
            ranges.push_back({batch.kind, first, batch.count, &batch});
        } else {
            const uint32_t first = static_cast<uint32_t>(glyphs.size());
            for (uint32_t i = 0; i < batch.count; ++i) {
                const Text &text = list.texts[nextText++];
                const Color &c = text.color;
                quads_.clear();
                font_.layout(text.text, text.baseline, text.size, quads_);
                for (const GlyphQuad &q : quads_)
                    glyphs.push_back({{q.x0, q.y0, q.x1, q.y1}, {q.u0, q.v0, q.u1, q.v1}, {c.r, c.g, c.b, c.a}});
            }
            ranges.push_back({batch.kind, first, static_cast<uint32_t>(glyphs.size()) - first, &batch});
        }
    }

    stats_.glyphs = static_cast<uint32_t>(glyphs.size());

    engine::Buffer &surfaceBuffer = surfaceBuffers_[renderer_.frameIndex()];
    engine::Buffer &glyphBuffer = glyphBuffers_[renderer_.frameIndex()];
    upload(renderer_, surfaceBuffer, instances);
    upload(renderer_, glyphBuffer, glyphs);
    engine::Buffer &imageBuffer = imageBuffers_[renderer_.frameIndex()];
    upload(renderer_, imageBuffer, imageQuads);
    stats_.uploadBytes = instances.size() * sizeof(Instance) + glyphs.size() * sizeof(GlyphInstance) +
                         imageQuads.size() * sizeof(GlyphInstance);

    const VkExtent2D extent = renderer_.extent();
    const float screen[2] = {static_cast<float>(extent.width), static_cast<float>(extent.height)};

    const engine::Pipeline *bound = nullptr;
    bool clipped = false;
    for (const Range &range : ranges) {
        // Scissor to the batch's clip, in whole pixels covering it
        const DrawList::Batch &batch = *range.batch;
        if (batch.clipped) {
            const float x0 = std::floor(batch.clip.x);
            const float y0 = std::floor(batch.clip.y);
            const float x1 = std::ceil(batch.clip.x + batch.clip.w);
            const float y1 = std::ceil(batch.clip.y + batch.clip.h);
            renderer_.setScissor(static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                                 static_cast<uint32_t>(std::max(0.0f, x1 - x0)), static_cast<uint32_t>(std::max(0.0f, y1 - y0)));
            clipped = true;
        } else if (clipped) {
            renderer_.clearScissor();
            clipped = false;
        }

        if (range.kind == DrawList::Kind::Image) {
            // One draw per image: each has its own texture
            for (uint32_t i = 0; i < range.count; ++i) {
                const engine::Pipeline &pipeline = shared_->imagePipeline;
                if (bound != &pipeline) {
                    renderer_.bind(pipeline);
                    ++stats_.pipelineBinds;
                    renderer_.pushConstants(pipeline, screen, sizeof(screen));
                    bound = &pipeline;
                }
                renderer_.bindDescriptorSet(pipeline, *imageSets[range.first + i]);
                renderer_.drawInstanced(imageBuffer, 6, range.first + i, 1);
                ++stats_.draws;
            }
            bound = nullptr; // the next text batch binds the atlas again
            continue;
        }
        const bool text = range.kind == DrawList::Kind::Text;
        const engine::Pipeline &pipeline = text ? shared_->textPipeline : shared_->surfacePipeline;
        if (bound != &pipeline) {
            renderer_.bind(pipeline);
            ++stats_.pipelineBinds;
            renderer_.pushConstants(pipeline, screen, sizeof(screen));
            if (text)
                renderer_.bindDescriptorSet(pipeline, shared_->atlasSet);
            bound = &pipeline;
        }
        renderer_.drawInstanced(text ? glyphBuffer : surfaceBuffer, 6, range.first, range.count);
        ++stats_.draws;
    }
    if (clipped)
        renderer_.clearScissor();
    stats_.submitMs = msSince(start);
}

const engine::DescriptorSet *UIRenderer::liveImage(const ImageData &image, uint32_t frame)
{
    LiveImage &live = images_[image.id];
    if (live.width != image.width || live.height != image.height || live.textures.empty()) {
        // A new size: new textures (the old ones may be in a frame in flight)
        if (!live.textures.empty())
            renderer_.waitIdle();
        live = LiveImage{};
        try {
            for (uint32_t i = 0; i < engine::Renderer::kMaxFramesInFlight; ++i)
                live.textures.push_back(renderer_.createLiveTexture(image.width, image.height, VK_FORMAT_B8G8R8A8_UNORM));
        } catch (const std::exception &) {
            imagesFailed_ = true;
            images_.erase(image.id);
            return nullptr;
        }
        live.pool = std::make_unique<engine::DescriptorPool>(renderer_.device(), engine::Renderer::kMaxFramesInFlight,
                                                             engine::Renderer::kMaxFramesInFlight);
        for (uint32_t i = 0; i < engine::Renderer::kMaxFramesInFlight; ++i) {
            live.sets.push_back(live.pool->allocate(shared_->atlasLayout));
            live.sets.back().writeTexture(0, live.textures[i]);
        }
        live.versions.assign(engine::Renderer::kMaxFramesInFlight, ~0ull);
        live.width = image.width;
        live.height = image.height;
    }
    if (live.versions[frame] != image.version && image.pixels.size() >= static_cast<size_t>(image.width) * image.height * 4) {
        live.textures[frame].write(image.pixels.data(), image.width, image.height, image.width * 4);
        live.versions[frame] = image.version;
    }
    live.lastFrame = frames_;
    return &live.sets[frame];
}

}
