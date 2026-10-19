#include "UIRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace ui {

namespace {

// Matches the instance inputs in shaders/text.vert (images only)
struct GlyphInstance {
    float rect[4];  // x0, y0, x1, y1
    float uv[4];    // u0, v0, u1, v1
    float color[4]; // rgb, alpha
};

constexpr VkDeviceSize kInitialPrimCapacity = 1024 * sizeof(Prim);
constexpr VkDeviceSize kInitialImageCapacity = 16 * sizeof(GlyphInstance);

// Matches the instance inputs of shaders/prim.vert: locations 0-6 vec4s, 7 the kind
engine::VertexLayout primLayout()
{
    engine::VertexLayout layout{
        sizeof(Prim),
        {
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, rect)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, clip)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, shape)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, base)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, light)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, dark)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, style)},
            {VK_FORMAT_R32_UINT, offsetof(Prim, kind)},
        },
    };
    layout.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    return layout;
}

// Matches the instance inputs in shaders/text.vert
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

    // The atlas is set 0 of the prim pipeline, and the images' sets use the same layout
    engine::Texture atlas;
    engine::DescriptorSetLayout atlasLayout;
    engine::DescriptorPool descriptorPool;
    engine::DescriptorSet atlasSet;

    engine::Shader primVertex;
    engine::Shader primFragment;
    engine::Pipeline primPipeline;

    engine::Shader textVertex;
    engine::Shader imageFragment;
    engine::Pipeline imagePipeline;

    Shared(engine::Renderer &renderer, const Font &f)
        : device(renderer.device()),
          format(renderer.format()),
          font(&f),
          atlas(renderer.createTexture(f.atlasWidth(), f.atlasHeight(), VK_FORMAT_R8_UNORM, f.atlas().data(), f.atlas().size())),
          atlasLayout(renderer.createDescriptorSetLayout({{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}})),
          descriptorPool(renderer.createDescriptorPool(1, 1)),
          atlasSet(descriptorPool.allocate(atlasLayout)),
          primVertex(renderer.createShader("prim.vert.spv", engine::ShaderStage::Vertex)),
          primFragment(renderer.createShader("prim.frag.spv", engine::ShaderStage::Fragment)),
          primPipeline(renderer.createPipeline(primVertex, primFragment, primLayout(), pipelineOptions(&atlasLayout))),
          textVertex(renderer.createShader("text.vert.spv", engine::ShaderStage::Vertex)),
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

    primBuffers_.reserve(engine::Renderer::kMaxFramesInFlight);
    imageBuffers_.reserve(engine::Renderer::kMaxFramesInFlight);
    for (uint32_t i = 0; i < engine::Renderer::kMaxFramesInFlight; ++i) {
        primBuffers_.push_back(renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kInitialPrimCapacity));
        imageBuffers_.push_back(renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kInitialImageCapacity));
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

    const uint32_t frame = renderer_.frameIndex();
    const std::vector<Prim> &prims = list.prims();
    const std::vector<DrawList::ImageRef> &refs = list.images();

    // First pass: every image's texture and quad, uploaded before anything is drawn (each buffer
    // is written once a frame). The images are in prim order, so the i-th image prim is refs[i];
    // an image that can't be shown keeps its place with a null set and is skipped when drawn.
    std::vector<GlyphInstance> imageQuads;
    std::vector<const engine::DescriptorSet *> imageSets;
    imageQuads.reserve(refs.size());
    imageSets.reserve(refs.size());
    for (const DrawList::ImageRef &ref : refs) {
        const Prim &p = prims[ref.prim];
        imageSets.push_back(imagesFailed_ ? nullptr : liveImage(*ref.image, frame));
        imageQuads.push_back({{p.rect[0], p.rect[1], p.rect[0] + p.rect[2], p.rect[1] + p.rect[3]},
                              {0.0f, 0.0f, 1.0f, 1.0f},
                              {1.0f, 1.0f, 1.0f, p.base[3]}});
    }

    engine::Buffer &primBuffer = primBuffers_[frame];
    engine::Buffer &imageBuffer = imageBuffers_[frame];
    upload(renderer_, primBuffer, prims);
    upload(renderer_, imageBuffer, imageQuads);
    stats_.uploadBytes = prims.size() * sizeof(Prim) + imageQuads.size() * sizeof(GlyphInstance);

    // Second pass: each maximal run of non-image prims is one draw (their clips are in the prims,
    // so no scissor); each image is a draw of its own, scissored to its clip
    const VkExtent2D extent = renderer_.extent();
    const float screen[2] = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    const uint32_t count = static_cast<uint32_t>(prims.size());
    const engine::Pipeline *bound = nullptr;
    size_t image = 0;
    uint32_t i = 0;
    while (i < count) {
        if (prims[i].kind != static_cast<uint32_t>(PrimKind::Image)) {
            uint32_t end = i;
            while (end < count && prims[end].kind != static_cast<uint32_t>(PrimKind::Image))
                ++end;
            const engine::Pipeline &pipeline = shared_->primPipeline;
            if (bound != &pipeline) {
                renderer_.bind(pipeline);
                ++stats_.pipelineBinds;
                renderer_.pushConstants(pipeline, screen, sizeof(screen));
                renderer_.bindDescriptorSet(pipeline, shared_->atlasSet);
                bound = &pipeline;
            }
            renderer_.drawInstanced(primBuffer, 6, i, end - i);
            ++stats_.draws;
            i = end;
            continue;
        }

        const engine::DescriptorSet *set = imageSets[image];
        const float *clip = prims[i].clip;
        ++i;
        const uint32_t quad = static_cast<uint32_t>(image++);
        if (!set)
            continue;
        const engine::Pipeline &pipeline = shared_->imagePipeline;
        if (bound != &pipeline) {
            renderer_.bind(pipeline);
            ++stats_.pipelineBinds;
            renderer_.pushConstants(pipeline, screen, sizeof(screen));
            bound = &pipeline;
        }
        renderer_.bindDescriptorSet(pipeline, *set);
        // The clip in whole pixels (a scissor's corner can't be negative); an unclipped image
        // gets the whole framebuffer
        if (clip[0] <= -kNoClip / 2) {
            renderer_.clearScissor();
        } else {
            const float x0 = std::max(0.0f, clip[0]);
            const float y0 = std::max(0.0f, clip[1]);
            const float x1 = std::max(x0, clip[2]);
            const float y1 = std::max(y0, clip[3]);
            renderer_.setScissor(static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                                 static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0));
        }
        renderer_.drawInstanced(imageBuffer, 6, quad, 1);
        ++stats_.draws;
        // Back to the whole framebuffer; the next run binds the prim pipeline again
        renderer_.clearScissor();
        bound = nullptr;
    }
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
