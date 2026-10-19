#include "Renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "Vulkan.h"

namespace engine {

namespace {

// Finds one queue family for graphics and one that can present to the surface
bool findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t &graphics, uint32_t &present)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    bool haveGraphics = false;
    bool havePresent = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!haveGraphics && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics = i;
            haveGraphics = true;
        }
        VkBool32 canPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &canPresent);
        if (!havePresent && canPresent) {
            present = i;
            havePresent = true;
        }
    }
    return haveGraphics && havePresent;
}

bool hasSwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

    bool found = false;
    for (const auto &extension : extensions)
        found = found || std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    if (!found)
        return false;

    uint32_t formatCount = 0;
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &modeCount, nullptr);
    return formatCount > 0 && modeCount > 0;
}

}

struct SharedDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;
    uint32_t presentFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    ~SharedDevice()
    {
        if (device != VK_NULL_HANDLE)
            vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);
    }
};

namespace {
// The device the live windows share (main thread only)
std::weak_ptr<SharedDevice> sharedDevice;
}

Renderer::Renderer(Window &window, bool vsync, bool deferSwapchain)
    : window_(window), vsync_(vsync)
{
    acquireDevice();
    createSwapchain(deferSwapchain);
    createRenderPass();
    createFramebuffers();
    createRenderFinishedSemaphores();
    createCommandResources();
    createFrameSyncObjects();
    setClearColor(0.0f, 0.0f, 0.0f);
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(device_);

    // Off-screen frame objects first: they use the command pool and the render pass
    destroyOffscreenTarget();
    if (offscreenPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_, offscreenPass_, nullptr);
    if (offscreenFence_ != VK_NULL_HANDLE)
        vkDestroyFence(device_, offscreenFence_, nullptr);
    if (offscreenCommands_ != VK_NULL_HANDLE)
        vkFreeCommandBuffers(device_, commandPool_, 1, &offscreenCommands_);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        vkDestroyFence(device_, inFlight_[i], nullptr);
    }
    vkDestroyCommandPool(device_, commandPool_, nullptr);
    destroySwapchain();
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    shared_.reset(); // the device and instance go with the last window
}

Shader Renderer::createShader(const std::string &file, ShaderStage stage) const
{
    return Shader(device_, std::string(ENGINE_SHADER_DIR) + "/" + file, stage);
}

Pipeline Renderer::createPipeline(const Shader &vertex, const Shader &fragment, const VertexLayout &layout,
                                  const PipelineOptions &options) const
{
    return Pipeline(device_, renderPass_, vertex, fragment, layout, options);
}

Buffer Renderer::createBuffer(VkBufferUsageFlags usage, VkDeviceSize capacity) const
{
    return Buffer(device_, physicalDevice_, usage, capacity);
}

Texture Renderer::createTexture(uint32_t width, uint32_t height, VkFormat format, const void *pixels, VkDeviceSize size) const
{
    Texture texture(device_, physicalDevice_, width, height, format);
    Buffer staging(device_, physicalDevice_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels, size, 1);

    // One-off command buffer: make the image a transfer target, copy, then make it sampleable
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &alloc, &cmd), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.handle(), texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    // The staging buffer is freed on return, so the copy must be finished first
    check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle");
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);

    return texture;
}

LiveTexture Renderer::createLiveTexture(uint32_t width, uint32_t height, VkFormat format) const
{
    if (!LiveTexture::supported(physicalDevice_, format))
        throw std::runtime_error("The graphics device can't show live images");
    LiveTexture texture(device_, physicalDevice_, width, height, format);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &alloc, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle");
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    return texture;
}

DescriptorSetLayout Renderer::createDescriptorSetLayout(const std::vector<DescriptorBinding> &bindings) const
{
    return DescriptorSetLayout(device_, bindings);
}

DescriptorPool Renderer::createDescriptorPool(uint32_t maxSets, uint32_t maxTextures, uint32_t maxBuffers) const
{
    return DescriptorPool(device_, maxSets, maxTextures, maxBuffers);
}

void Renderer::setClearColor(float r, float g, float b, float a)
{
    clearColor_.color = {{r, g, b, a}};
}

namespace {

double millisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}

void Renderer::beginFrame()
{
    auto waitStart = std::chrono::steady_clock::now();
    check(vkWaitForFences(device_, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX), "vkWaitForFences");
    waitMs_ = millisecondsSince(waitStart);

    // Window was resized or minimised: rebuild the swapchain before acquiring from it
    int width = 0;
    int height = 0;
    window_.framebufferSize(width, height);
    if (static_cast<uint32_t>(width) != extent_.width || static_cast<uint32_t>(height) != extent_.height)
        recreateSwapchain();

    waitStart = std::chrono::steady_clock::now();
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[frame_], VK_NULL_HANDLE, &imageIndex_);
    waitMs_ += millisecondsSince(waitStart);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[frame_], VK_NULL_HANDLE, &imageIndex_);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        check(result, "vkAcquireNextImageKHR");

    check(vkResetFences(device_, 1, &inFlight_[frame_]), "vkResetFences");

    VkCommandBuffer cmd = commandBuffers_[frame_];
    check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = renderPass_;
    pass.framebuffer = framebuffers_[imageIndex_];
    pass.renderArea.extent = extent_;
    pass.clearValueCount = 1;
    pass.pClearValues = &clearColor_;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

VkCommandBuffer Renderer::recording() const
{
    return offscreen_ ? offscreenCommands_ : commandBuffers_[frame_];
}

void Renderer::bind(const Pipeline &pipeline)
{
    vkCmdBindPipeline(recording(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
}

void Renderer::draw(const Buffer &vertexBuffer)
{
    VkCommandBuffer cmd = recording();
    VkBuffer buffers[] = {vertexBuffer.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdDraw(cmd, vertexBuffer.count(), 1, 0, 0);
}

void Renderer::drawInstanced(const Buffer &instances, uint32_t vertexCount)
{
    drawInstanced(instances, vertexCount, 0, instances.count());
}

void Renderer::drawInstanced(const Buffer &instances, uint32_t vertexCount, uint32_t firstInstance, uint32_t instanceCount)
{
    if (instanceCount == 0)
        return;
    VkCommandBuffer cmd = recording();
    VkBuffer buffers[] = {instances.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdDraw(cmd, vertexCount, instanceCount, 0, firstInstance);
}

void Renderer::bindDescriptorSet(const Pipeline &pipeline, const DescriptorSet &set, uint32_t index)
{
    VkDescriptorSet handle = set.handle();
    vkCmdBindDescriptorSets(recording(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            index, 1, &handle, 0, nullptr);
}

void Renderer::pushConstants(const Pipeline &pipeline, const void *data, uint32_t size)
{
    vkCmdPushConstants(recording(), pipeline.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void Renderer::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    // Vulkan requires a non-negative offset; clamp the rectangle to the framebuffer being drawn
    const VkExtent2D size = extent();
    const int64_t x0 = std::clamp<int64_t>(x, 0, size.width);
    const int64_t y0 = std::clamp<int64_t>(y, 0, size.height);
    const int64_t x1 = std::clamp<int64_t>(static_cast<int64_t>(x) + width, x0, size.width);
    const int64_t y1 = std::clamp<int64_t>(static_cast<int64_t>(y) + height, y0, size.height);

    VkRect2D scissor{};
    scissor.offset = {static_cast<int32_t>(x0), static_cast<int32_t>(y0)};
    scissor.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
    vkCmdSetScissor(recording(), 0, 1, &scissor);
}

void Renderer::clearScissor()
{
    const VkExtent2D size = extent();
    setScissor(0, 0, size.width, size.height);
}

void Renderer::endFrame()
{
    VkCommandBuffer cmd = commandBuffers_[frame_];
    vkCmdEndRenderPass(cmd);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_[frame_];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_[imageIndex_];
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, inFlight_[frame_]), "vkQueueSubmit");

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex_];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex_;
    const auto waitStart = std::chrono::steady_clock::now();
    VkResult result = vkQueuePresentKHR(presentQueue_, &present);
    waitMs_ += millisecondsSince(waitStart);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        recreateSwapchain();
    else
        check(result, "vkQueuePresentKHR");

    frame_ = (frame_ + 1) % kMaxFramesInFlight;
}

void Renderer::beginOffscreen(uint32_t width, uint32_t height)
{
    if (offscreen_)
        throw std::runtime_error("beginOffscreen called while an off-screen frame is already recording");
    if (width == 0 || height == 0)
        throw std::runtime_error("An off-screen frame needs a width and height of at least 1");

    // The render pass is made once. It doesn't depend on the swapchain: createRenderPass runs in
    // the constructor even when the swapchain is deferred, so swapchainFormat_ is always set.
    if (offscreenPass_ == VK_NULL_HANDLE) {
        VkAttachmentDescription color{};
        color.format = swapchainFormat_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        // Same as renderPass_, plus: the colour writes are visible to the copy that follows
        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &color;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = dependencies;
        check(vkCreateRenderPass(device_, &info, nullptr, &offscreenPass_), "vkCreateRenderPass");
    }

    // The image, its view and framebuffer and the readback buffer are kept while the size holds
    if (offscreenImage_ == VK_NULL_HANDLE || offscreenExtent_.width != width || offscreenExtent_.height != height) {
        destroyOffscreenTarget();

        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = swapchainFormat_;
        image.extent = {width, height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &image, nullptr, &offscreenImage_), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, offscreenImage_, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = findMemoryType(physicalDevice_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &alloc, nullptr, &offscreenMemory_), "vkAllocateMemory");
        check(vkBindImageMemory(device_, offscreenImage_, offscreenMemory_, 0), "vkBindImageMemory");

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = offscreenImage_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swapchainFormat_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &view, nullptr, &offscreenView_), "vkCreateImageView");

        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = offscreenPass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &offscreenView_;
        framebuffer.width = width;
        framebuffer.height = height;
        framebuffer.layers = 1;
        check(vkCreateFramebuffer(device_, &framebuffer, nullptr, &offscreenFramebuffer_), "vkCreateFramebuffer");

        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = static_cast<VkDeviceSize>(width) * height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &buffer, nullptr, &readbackBuffer_), "vkCreateBuffer");

        VkMemoryRequirements bufferRequirements{};
        vkGetBufferMemoryRequirements(device_, readbackBuffer_, &bufferRequirements);
        VkMemoryAllocateInfo bufferAlloc{};
        bufferAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bufferAlloc.allocationSize = bufferRequirements.size;
        bufferAlloc.memoryTypeIndex = findMemoryType(physicalDevice_, bufferRequirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device_, &bufferAlloc, nullptr, &readbackMemory_), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, readbackBuffer_, readbackMemory_, 0), "vkBindBufferMemory");
    }
    offscreenExtent_ = {width, height};

    if (offscreenCommands_ == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = commandPool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &alloc, &offscreenCommands_), "vkAllocateCommandBuffers");
    }
    if (offscreenFence_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fence, nullptr, &offscreenFence_), "vkCreateFence");
    }

    // Don't touch this slot's per-frame buffers while an on-screen frame still uses them
    check(vkWaitForFences(device_, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX), "vkWaitForFences");

    check(vkResetCommandBuffer(offscreenCommands_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(offscreenCommands_, &begin), "vkBeginCommandBuffer");

    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = offscreenPass_;
    pass.framebuffer = offscreenFramebuffer_;
    pass.renderArea.extent = offscreenExtent_;
    pass.clearValueCount = 1;
    pass.pClearValues = &clearColor_;
    vkCmdBeginRenderPass(offscreenCommands_, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(offscreenCommands_, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = offscreenExtent_;
    vkCmdSetScissor(offscreenCommands_, 0, 1, &scissor);

    offscreen_ = true;
}

std::vector<uint8_t> Renderer::endOffscreen()
{
    if (!offscreen_)
        throw std::runtime_error("endOffscreen called without beginOffscreen");
    offscreen_ = false;

    const VkExtent2D size = offscreenExtent_;
    VkCommandBuffer cmd = offscreenCommands_;
    vkCmdEndRenderPass(cmd);

    // The image is now TRANSFER_SRC_OPTIMAL: copy all of it into the readback buffer, packed
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {size.width, size.height, 1};
    vkCmdCopyImageToBuffer(cmd, offscreenImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_, 1, &region);

    // Make the copy visible to the host read below
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = readbackBuffer_;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, offscreenFence_), "vkQueueSubmit");
    check(vkWaitForFences(device_, 1, &offscreenFence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(vkResetFences(device_, 1, &offscreenFence_), "vkResetFences");

    const size_t bytes = static_cast<size_t>(size.width) * size.height * 4;
    std::vector<uint8_t> pixels(bytes);
    void *mapped = nullptr;
    check(vkMapMemory(device_, readbackMemory_, 0, bytes, 0, &mapped), "vkMapMemory");
    std::memcpy(pixels.data(), mapped, bytes);
    vkUnmapMemory(device_, readbackMemory_);
    return pixels;
}

void Renderer::destroyOffscreenTarget()
{
    if (offscreenFramebuffer_ != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device_, offscreenFramebuffer_, nullptr);
    offscreenFramebuffer_ = VK_NULL_HANDLE;

    if (offscreenView_ != VK_NULL_HANDLE)
        vkDestroyImageView(device_, offscreenView_, nullptr);
    offscreenView_ = VK_NULL_HANDLE;

    if (offscreenImage_ != VK_NULL_HANDLE)
        vkDestroyImage(device_, offscreenImage_, nullptr);
    offscreenImage_ = VK_NULL_HANDLE;

    if (offscreenMemory_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, offscreenMemory_, nullptr);
    offscreenMemory_ = VK_NULL_HANDLE;

    if (readbackBuffer_ != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, readbackBuffer_, nullptr);
    readbackBuffer_ = VK_NULL_HANDLE;

    if (readbackMemory_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, readbackMemory_, nullptr);
    readbackMemory_ = VK_NULL_HANDLE;
}

void Renderer::waitIdle()
{
    vkDeviceWaitIdle(device_);
}

namespace {
bool overlaysAllowed = false;
}

void Renderer::setOverlaysAllowed(bool allowed)
{
    overlaysAllowed = allowed;
}

void Renderer::acquireDevice()
{
    // The instance first: the surface is made on it, and the device is picked for the surface
    shared_ = sharedDevice.lock();
    if (!shared_) {
        shared_ = std::make_shared<SharedDevice>();
        sharedDevice = shared_;

        // The loader reads this when the instance is made
        if (!overlaysAllowed && !std::getenv("VK_LOADER_LAYERS_DISABLE") && !std::getenv("VK_LOADER_LAYERS_ENABLE")) {
#ifdef _WIN32
            _putenv_s("VK_LOADER_LAYERS_DISABLE", "~implicit~");
#else
            setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 0);
#endif
        }

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "UI Engine";
        app.apiVersion = VK_API_VERSION_1_0;

        // Surface extensions that GLFW needs on this platform
        uint32_t extensionCount = 0;
        const char **extensions = glfwGetRequiredInstanceExtensions(&extensionCount);

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = extensionCount;
        info.ppEnabledExtensionNames = extensions;
        check(vkCreateInstance(&info, nullptr, &shared_->instance), "vkCreateInstance");
    }
    instance_ = shared_->instance;
    check(glfwCreateWindowSurface(instance_, window_.handle(), nullptr, &surface_), "glfwCreateWindowSurface");

    SharedDevice &gpu = *shared_;
    if (gpu.device == VK_NULL_HANDLE) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0)
            throw std::runtime_error("No Vulkan-capable GPU found");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (VkPhysicalDevice device : devices) {
            if (findQueueFamilies(device, surface_, gpu.graphicsFamily, gpu.presentFamily) && hasSwapchainSupport(device, surface_)) {
                gpu.physical = device;
                break;
            }
        }
        if (gpu.physical == VK_NULL_HANDLE)
            throw std::runtime_error("No GPU with graphics, presentation and swapchain support");

        // One queue per unique family (graphics and present may be the same)
        const std::set<uint32_t> families = {gpu.graphicsFamily, gpu.presentFamily};
        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        for (uint32_t family : families) {
            VkDeviceQueueCreateInfo queue{};
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            queues.push_back(queue);
        }

        VkPhysicalDeviceFeatures features{};
        const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        info.pQueueCreateInfos = queues.data();
        info.pEnabledFeatures = &features;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        check(vkCreateDevice(gpu.physical, &info, nullptr, &gpu.device), "vkCreateDevice");
        vkGetDeviceQueue(gpu.device, gpu.graphicsFamily, 0, &gpu.graphicsQueue);
        vkGetDeviceQueue(gpu.device, gpu.presentFamily, 0, &gpu.presentQueue);
    } else {
        // Every window presents from the same queue, so it must be able to reach this surface
        VkBool32 canPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physical, gpu.presentFamily, surface_, &canPresent);
        if (!canPresent || !hasSwapchainSupport(gpu.physical, surface_)) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            throw std::runtime_error("The GPU can't present to this window");
        }
    }
    physicalDevice_ = gpu.physical;
    device_ = gpu.device;
    graphicsFamily_ = gpu.graphicsFamily;
    presentFamily_ = gpu.presentFamily;
    graphicsQueue_ = gpu.graphicsQueue;
    presentQueue_ = gpu.presentQueue;
}

void Renderer::createSwapchain(bool formatOnly)
{
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR format = formats[0];
    for (const auto &candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format = candidate;
            break;
        }
    }
    swapchainFormat_ = format.format;
    if (formatOnly) {
        extent_ = {}; // beginFrame makes it
        return;
    }

    // The surface dictates the size, except when it leaves it to us (UINT32_MAX)
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0;
        int height = 0;
        window_.framebufferSize(width, height);
        extent.width = std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    extent_ = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const uint32_t families[] = {graphicsFamily_, presentFamily_};
    if (graphicsFamily_ != presentFamily_) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; always supported
    if (!vsync_) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());
        for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
                info.presentMode = preferred;
                break;
            }
        }
    }
    info.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    uint32_t imageTotal = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageTotal, nullptr);
    std::vector<VkImage> images(imageTotal);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageTotal, images.data());

    imageViews_.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swapchainFormat_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &view, nullptr, &imageViews_[i]), "vkCreateImageView");
    }
}

void Renderer::createRenderPass()
{
    // One colour target: cleared on load, stored, and handed to the presentation engine
    VkAttachmentDescription color{};
    color.format = swapchainFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // Wait for the presentation engine to release the image before writing it
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    check(vkCreateRenderPass(device_, &info, nullptr, &renderPass_), "vkCreateRenderPass");
}

void Renderer::createFramebuffers()
{
    framebuffers_.resize(imageViews_.size());
    for (size_t i = 0; i < imageViews_.size(); ++i) {
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &imageViews_[i];
        info.width = extent_.width;
        info.height = extent_.height;
        info.layers = 1;
        check(vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]), "vkCreateFramebuffer");
    }
}

void Renderer::createRenderFinishedSemaphores()
{
    renderFinished_.resize(imageViews_.size());
    for (auto &semaphore : renderFinished_) {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(device_, &info, nullptr, &semaphore), "vkCreateSemaphore");
    }
}

void Renderer::createCommandResources()
{
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = graphicsFamily_;
    check(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "vkCreateCommandPool");

    commandBuffers_.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kMaxFramesInFlight;
    check(vkAllocateCommandBuffers(device_, &alloc, commandBuffers_.data()), "vkAllocateCommandBuffers");
}

void Renderer::createFrameSyncObjects()
{
    imageAvailable_.resize(kMaxFramesInFlight);
    inFlight_.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    // Start signalled so the first beginFrame doesn't wait forever
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        check(vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_[i]), "vkCreateSemaphore");
        check(vkCreateFence(device_, &fence, nullptr, &inFlight_[i]), "vkCreateFence");
    }
}

void Renderer::releaseSwapchain()
{
    if (swapchain_ == VK_NULL_HANDLE)
        return;
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    extent_ = {}; // so beginFrame makes it again
    ++swapchainGeneration_;
}

void Renderer::destroySwapchain()
{
    for (VkSemaphore semaphore : renderFinished_)
        vkDestroySemaphore(device_, semaphore, nullptr);
    renderFinished_.clear();

    for (VkFramebuffer framebuffer : framebuffers_)
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    framebuffers_.clear();

    for (VkImageView view : imageViews_)
        vkDestroyImageView(device_, view, nullptr);
    imageViews_.clear();

    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Renderer::recreateSwapchain()
{
    // Pause while minimised: a zero-sized swapchain is invalid
    int width = 0;
    int height = 0;
    window_.framebufferSize(width, height);
    while (width == 0 || height == 0) {
        window_.waitEvents();
        window_.framebufferSize(width, height);
    }

    vkDeviceWaitIdle(device_);
    destroySwapchain();
    createSwapchain();
    createFramebuffers();
    createRenderFinishedSemaphores();
    ++swapchainGeneration_;
}

}
