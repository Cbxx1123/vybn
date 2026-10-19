#include "Texture.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "Vulkan.h"

namespace engine {

Texture::Texture(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkFormat format)
    : device_(device), width_(width), height_(height)
{
    if (width == 0 || height == 0)
        throw std::runtime_error("Cannot create an empty texture");

    // Optimal tiling: only reachable through a transfer, which Renderer::createTexture records
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &info, nullptr, &image_), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = findMemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &memory_), "vkAllocateMemory");
    check(vkBindImageMemory(device_, image_, memory_, 0), "vkBindImageMemory");

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    check(vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView");

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    check(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler");
}

Texture::~Texture()
{
    release();
}

Texture::Texture(Texture &&other) noexcept
{
    *this = std::move(other);
}

Texture &Texture::operator=(Texture &&other) noexcept
{
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
    }
    return *this;
}

// LiveTexture

LiveTexture::LiveTexture(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkFormat format)
    : device_(device), width_(width), height_(height)
{
    if (width == 0 || height == 0)
        throw std::runtime_error("Cannot create an empty texture");
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_LINEAR;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    check(vkCreateImage(device_, &info, nullptr, &image_), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex =
        findMemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &memory_), "vkAllocateMemory");
    check(vkBindImageMemory(device_, image_, memory_, 0), "vkBindImageMemory");
    check(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");

    VkImageSubresource sub{};
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(device_, image_, &sub, &layout);
    rowPitch_ = layout.rowPitch;
    offset_ = layout.offset;
    std::memset(mapped_, 0, static_cast<size_t>(requirements.size));

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    check(vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView");

    // Nearest: a plugin's window is shown pixel for pixel
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler");
}

LiveTexture::~LiveTexture() { release(); }

LiveTexture::LiveTexture(LiveTexture &&other) noexcept { *this = std::move(other); }

LiveTexture &LiveTexture::operator=(LiveTexture &&other) noexcept
{
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
        mapped_ = std::exchange(other.mapped_, nullptr);
        rowPitch_ = std::exchange(other.rowPitch_, 0);
        offset_ = std::exchange(other.offset_, 0);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
    }
    return *this;
}

void LiveTexture::write(const void *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
    if (!mapped_ || !pixels)
        return;
    width = std::min(width, width_);
    height = std::min(height, height_);
    const char *from = static_cast<const char *>(pixels);
    char *to = static_cast<char *>(mapped_) + offset_;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(to + y * rowPitch_, from + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
}

bool LiveTexture::supported(VkPhysicalDevice physical, VkFormat format)
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return (properties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

void LiveTexture::release()
{
    if (device_ == VK_NULL_HANDLE)
        return;
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr); // unmaps it
    device_ = VK_NULL_HANDLE;
}

void Texture::release()
{
    if (device_ == VK_NULL_HANDLE)
        return;
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    device_ = VK_NULL_HANDLE;
}

}
