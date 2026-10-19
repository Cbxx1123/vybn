#pragma once

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "Buffer.h"
#include "Descriptors.h"
#include "Pipeline.h"
#include "Shader.h"
#include "Texture.h"
#include "Window.h"

namespace engine {

// The Vulkan instance and device every window draws with: made by the first Renderer and
// destroyed with the last, so a window costs only its surface and swapchain
struct SharedDevice;

// Owns the swapchain and frame loop for one window, on the device all windows share.
// Shaders, pipelines, buffers, textures and descriptor objects it creates must be destroyed
// before the Renderer.
class Renderer {
public:
    static constexpr uint32_t kMaxFramesInFlight = 2;

    // With `vsync` false the swapchain prefers a mode that doesn't wait for the display
    // (mailbox, then immediate), so extra windows don't slow the main one down
    // With `deferSwapchain` the swapchain is made by the first beginFrame instead
    explicit Renderer(Window &window, bool vsync = true, bool deferSwapchain = false);

    // Whether Vulkan's implicit layers (overlays and capture tools other programs install, such
    // as Steam's, OBS's or vkconfig's) are loaded into the app. Call before the first Renderer.
    // Off by default: each one costs memory and start-up time. A VK_LOADER_LAYERS_* variable the
    // user set wins.
    static void setOverlaysAllowed(bool allowed);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // `file` is relative to the shaders directory (ENGINE_SHADER_DIR, the output/shaders folder)
    Shader createShader(const std::string &file, ShaderStage stage) const;
    Pipeline createPipeline(const Shader &vertex, const Shader &fragment, const VertexLayout &layout,
                            const PipelineOptions &options = {}) const;

    template <typename T>
    VertexBuffer<T> createVertexBuffer(const std::vector<T> &vertices) const
    {
        return VertexBuffer<T>(device_, physicalDevice_, vertices);
    }

    // Empty buffer for data rewritten at runtime with Buffer::write
    Buffer createBuffer(VkBufferUsageFlags usage, VkDeviceSize capacity) const;

    // Device-local texture filled with `size` bytes of tightly packed `pixels` through a staging
    // buffer. Blocks until the upload finishes, so call it at load time, not mid-frame.
    Texture createTexture(uint32_t width, uint32_t height, VkFormat format, const void *pixels, VkDeviceSize size) const;
    // A texture rewritten while in use (see LiveTexture), moved into the GENERAL layout once
    // (blocking, like createTexture). Throws if the device can't sample linear images of `format`.
    LiveTexture createLiveTexture(uint32_t width, uint32_t height, VkFormat format) const;
    DescriptorSetLayout createDescriptorSetLayout(const std::vector<DescriptorBinding> &bindings) const;
    DescriptorPool createDescriptorPool(uint32_t maxSets, uint32_t maxTextures, uint32_t maxBuffers = 0) const;

    void setClearColor(float r, float g, float b, float a = 1.0f);

    // Frame loop: beginFrame, then bind and draw, then endFrame
    void beginFrame();
    void bind(const Pipeline &pipeline);
    void draw(const Buffer &vertexBuffer);
    // Draws `vertexCount` vertices once per element of `instances` (bound at binding 0)
    void drawInstanced(const Buffer &instances, uint32_t vertexCount);
    // Same, for `instanceCount` elements starting at `firstInstance`
    void drawInstanced(const Buffer &instances, uint32_t vertexCount, uint32_t firstInstance, uint32_t instanceCount);
    // Binds `set` as set `index` of `pipeline`'s layout; call after bind(). Sets bound for one
    // pipeline stay bound for another whose layout matches up to that set.
    void bindDescriptorSet(const Pipeline &pipeline, const DescriptorSet &set, uint32_t index = 0);
    void pushConstants(const Pipeline &pipeline, const void *data, uint32_t size);
    // Limits drawing to a rectangle in framebuffer pixels (clamped to the framebuffer).
    // beginFrame resets it to the whole framebuffer; clearScissor does the same mid-frame.
    void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);
    void clearScissor();
    void endFrame();

    // Off-screen frames, for tests (the render self-test's golden images): beginOffscreen starts
    // recording into an image of its own, `width` x `height` in the swapchain's format, cleared to
    // the clear colour; bind and draw work as between beginFrame and endFrame, and extent() is
    // the image's size until endOffscreen. endOffscreen finishes, waits for the GPU and returns
    // the pixels, 4 bytes each (blue, green, red, alpha, as stored: sRGB-encoded), rows packed,
    // top row first. Works whether or not the window has a swapchain (a hidden window made with
    // deferSwapchain), and doesn't touch it. Not between beginFrame and endFrame.
    void beginOffscreen(uint32_t width, uint32_t height);
    std::vector<uint8_t> endOffscreen();

    void waitIdle();

    // Lets go of the swapchain (its images and framebuffers) while the window isn't drawn, as
    // a hidden spare window; the next beginFrame makes it again at the window's size
    void releaseSwapchain();
    bool hasSwapchain() const { return swapchain_ != VK_NULL_HANDLE; }
    // Counts the swapchains made and let go, so a caller can tell that the window's images were
    // replaced since it last presented (they are blank until drawn again)
    uint64_t swapchainGeneration() const { return swapchainGeneration_; }

    // Milliseconds the last frame spent blocked: waiting for a frame in flight to finish, for a
    // swapchain image, and in present (with vsync, mostly waiting for the display)
    double lastWaitMs() const { return waitMs_; }

    // Index of the frame being recorded, in [0, kMaxFramesInFlight). Use it to pick per-frame buffers.
    uint32_t frameIndex() const { return frame_; }
    // The size being drawn: the swapchain's, or the off-screen image's between beginOffscreen and
    // endOffscreen
    VkExtent2D extent() const { return offscreen_ ? offscreenExtent_ : extent_; }
    // The device (shared by every window) and this window's swapchain format: resources made
    // through one Renderer work with another that has the same pair
    VkDevice device() const { return device_; }
    VkFormat format() const { return swapchainFormat_; }

private:
    // The shared instance and device (made on first use), and this window's surface on it
    void acquireDevice();
    // `formatOnly` picks the format (for the render pass) without making the swapchain
    void createSwapchain(bool formatOnly = false);
    void createRenderPass();
    void createFramebuffers();
    void createRenderFinishedSemaphores();
    void createCommandResources();
    void createFrameSyncObjects();
    void destroySwapchain();
    void recreateSwapchain();

    Window &window_;
    bool vsync_;

    std::shared_ptr<SharedDevice> shared_;
    // Copies of the shared handles
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    uint32_t presentFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImageView> imageViews_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinished_; // one per swapchain image

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    // Per frame in flight
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkFence> inFlight_;
    uint32_t frame_ = 0;
    uint32_t imageIndex_ = 0;

    VkClearValue clearColor_{};
    double waitMs_ = 0.0;
    uint64_t swapchainGeneration_ = 0;

    // The command buffer being recorded: this frame's, or the off-screen one
    VkCommandBuffer recording() const;

    // Off-screen frames (beginOffscreen): a render pass like renderPass_ (compatible, so every
    // pipeline works with it) ending in TRANSFER_SRC, the image and its memory, view and
    // framebuffer (kept for the next frame of the same size), a host-visible buffer to read it
    // back into, and a command buffer and fence of their own
    void destroyOffscreenTarget();
    VkRenderPass offscreenPass_ = VK_NULL_HANDLE;
    VkImage offscreenImage_ = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory_ = VK_NULL_HANDLE;
    VkImageView offscreenView_ = VK_NULL_HANDLE;
    VkFramebuffer offscreenFramebuffer_ = VK_NULL_HANDLE;
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkCommandBuffer offscreenCommands_ = VK_NULL_HANDLE;
    VkFence offscreenFence_ = VK_NULL_HANDLE;
    VkExtent2D offscreenExtent_{};
    bool offscreen_ = false; // between beginOffscreen and endOffscreen: draws go to offscreenCommands_
};

}
