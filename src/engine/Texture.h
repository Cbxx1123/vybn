#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace engine {

// A sampled 2D image in device-local memory, with its view and a linear, clamp-to-edge sampler.
// Created and filled through Renderer::createTexture (a staging upload).
// Movable, not copyable; must be destroyed before its Renderer.
class Texture {
public:
    Texture(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkFormat format);
    ~Texture();

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    Texture(Texture &&other) noexcept;
    Texture &operator=(Texture &&other) noexcept;

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    void release();

    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

// A texture the CPU rewrites while it is in use (a plugin's window, drawn into the UI): a
// linear-tiled image in host-visible, coherent memory, kept mapped and in the GENERAL layout, so
// write() is a plain copy with no commands or waiting. The caller keeps one per frame in flight
// and writes the one for this frame after Renderer::beginFrame (whose fence wait means the GPU
// is done with it). Made through Renderer::createLiveTexture. Movable, not copyable.
class LiveTexture {
public:
    LiveTexture(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkFormat format);
    ~LiveTexture();

    LiveTexture(const LiveTexture &) = delete;
    LiveTexture &operator=(const LiveTexture &) = delete;
    LiveTexture(LiveTexture &&other) noexcept;
    LiveTexture &operator=(LiveTexture &&other) noexcept;

    // Copies `height` rows of `width` 4-byte pixels, `stride` bytes apart (at most its size)
    void write(const void *pixels, uint32_t width, uint32_t height, uint32_t stride);

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Whether the device can sample linear images of `format` (B8G8R8A8 everywhere on desktops)
    static bool supported(VkPhysicalDevice physical, VkFormat format);

private:
    void release();

    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    void *mapped_ = nullptr;
    VkDeviceSize rowPitch_ = 0;
    VkDeviceSize offset_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}
