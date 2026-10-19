#include "UIRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace ui {

namespace {

constexpr VkDeviceSize kInitialPrimCapacity = 1024 * sizeof(Prim);

// The picture set 1 shows when no image is drawn: one transparent pixel
const uint8_t kBlankPixel[4] = {0, 0, 0, 0};

// Matches the instance inputs of shaders/prim.vert: locations 0-3 vec4s (rect, clip, shape, style),
// 4 the uvec4 of packed base, light, dark and kind
engine::VertexLayout primLayout()
{
    engine::VertexLayout layout{
        sizeof(Prim),
        {
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, rect)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, clip)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, shape)},
            {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Prim, style)},
            {VK_FORMAT_R32G32B32A32_UINT, offsetof(Prim, base)},
        },
    };
    layout.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    return layout;
}

// Set 0 is the atlas and set 1 the image; both are one combined image sampler, so one layout does
engine::PipelineOptions pipelineOptions(const engine::DescriptorSetLayout &samplerLayout)
{
    engine::PipelineOptions options;
    options.alphaBlending = true;
    options.pushConstantSize = 2 * sizeof(float); // screen size
    options.descriptorSetLayouts = {&samplerLayout, &samplerLayout};
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

    // Set 0 of the prim pipeline is the atlas, and set 1 the image; both sets use the same layout
    engine::Texture atlas;
    engine::DescriptorSetLayout atlasLayout;
    engine::DescriptorPool descriptorPool;
    engine::DescriptorSet atlasSet;

    // Set 1 when no image is drawn, so it is always bound to something
    engine::Texture blank;
    engine::DescriptorSet blankSet;

    engine::Shader primVertex;
    engine::Shader primFragment;
    engine::Pipeline primPipeline;

    Shared(engine::Renderer &renderer, const Font &f)
        : device(renderer.device()),
          format(renderer.format()),
          font(&f),
          atlas(renderer.createTexture(f.atlasWidth(), f.atlasHeight(), VK_FORMAT_R8_UNORM, f.atlas().data(), f.atlas().size())),
          atlasLayout(renderer.createDescriptorSetLayout({{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}})),
          descriptorPool(renderer.createDescriptorPool(2, 2)),
          atlasSet(descriptorPool.allocate(atlasLayout)),
          blank(renderer.createTexture(1, 1, VK_FORMAT_B8G8R8A8_UNORM, kBlankPixel, sizeof(kBlankPixel))),
          blankSet(descriptorPool.allocate(atlasLayout)),
          primVertex(renderer.createShader("prim.vert.spv", engine::ShaderStage::Vertex)),
          primFragment(renderer.createShader("prim.frag.spv", engine::ShaderStage::Fragment)),
          primPipeline(renderer.createPipeline(primVertex, primFragment, primLayout(), pipelineOptions(atlasLayout)))
    {
        atlasSet.writeTexture(0, atlas);
        blankSet.writeTexture(0, blank);
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
    for (uint32_t i = 0; i < engine::Renderer::kMaxFramesInFlight; ++i)
        primBuffers_.push_back(renderer.createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kInitialPrimCapacity));
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

    // First pass: every image's texture, brought up to date before anything is drawn (each buffer
    // is written once a frame). The images are in prim order, so the i-th image prim is refs[i];
    // an image that can't be shown keeps its place with a null set and is skipped when drawn.
    imageSets_.clear();
    for (const DrawList::ImageRef &ref : refs)
        imageSets_.push_back(imagesFailed_ ? nullptr : liveImage(*ref.image, frame));

    engine::Buffer &primBuffer = primBuffers_[frame];
    upload(renderer_, primBuffer, prims);
    stats_.uploadBytes = prims.size() * sizeof(Prim);

    // Second pass: the pipeline once, with the atlas at set 0 and the blank picture at set 1. Each
    // maximal run of non-image prims is one draw; each image is a draw of its own with its picture
    // at set 1. The clips are in the prims, so there are no scissors.
    const VkExtent2D extent = renderer_.extent();
    const float screen[2] = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    const engine::Pipeline &pipeline = shared_->primPipeline;
    renderer_.bind(pipeline);
    ++stats_.pipelineBinds;
    renderer_.pushConstants(pipeline, screen, sizeof(screen));
    renderer_.bindDescriptorSet(pipeline, shared_->atlasSet, 0);
    renderer_.bindDescriptorSet(pipeline, shared_->blankSet, 1);

    const uint32_t count = static_cast<uint32_t>(prims.size());
    size_t image = 0;
    uint32_t i = 0;
    while (i < count) {
        if (prims[i].kind != static_cast<uint32_t>(PrimKind::Image)) {
            uint32_t end = i;
            while (end < count && prims[end].kind != static_cast<uint32_t>(PrimKind::Image))
                ++end;
            renderer_.drawInstanced(primBuffer, 6, i, end - i);
            ++stats_.draws;
            i = end;
            continue;
        }

        const engine::DescriptorSet *set = image < imageSets_.size() ? imageSets_[image] : nullptr;
        ++image;
        if (set) {
            renderer_.bindDescriptorSet(pipeline, *set, 1);
            renderer_.drawInstanced(primBuffer, 6, i, 1);
            ++stats_.draws;
        }
        ++i;
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
