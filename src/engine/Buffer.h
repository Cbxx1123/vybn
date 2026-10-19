#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace engine {

// A persistently mapped buffer in host-visible GPU memory. Contents can be replaced with write().
// Movable, not copyable; must be destroyed before its Renderer.
class Buffer {
public:
    // Empty buffer with room for `capacity` bytes
    Buffer(VkDevice device, VkPhysicalDevice physical, VkBufferUsageFlags usage, VkDeviceSize capacity);
    // Buffer filled with `size` bytes of `data`, holding `count` elements
    Buffer(VkDevice device, VkPhysicalDevice physical, VkBufferUsageFlags usage, const void *data, VkDeviceSize size, uint32_t count);
    virtual ~Buffer();

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;

    // Replaces the contents. `size` must not exceed capacity(). Only call while the GPU
    // isn't reading this buffer (for per-frame data: after Renderer::beginFrame).
    void write(const void *data, VkDeviceSize size, uint32_t count);

    VkBuffer handle() const { return buffer_; }
    uint32_t count() const { return count_; }
    VkDeviceSize capacity() const { return capacity_; }

private:
    void release();

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void *mapped_ = nullptr;
    VkDeviceSize capacity_ = 0;
    uint32_t count_ = 0;
};

template <typename T>
class VertexBuffer : public Buffer {
public:
    VertexBuffer(VkDevice device, VkPhysicalDevice physical, const std::vector<T> &vertices)
        : Buffer(device, physical, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(),
                 sizeof(T) * vertices.size(), static_cast<uint32_t>(vertices.size()))
    {
    }
};

}
