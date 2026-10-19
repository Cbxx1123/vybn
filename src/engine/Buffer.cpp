#include "Buffer.h"

#include <cstring>
#include <stdexcept>
#include <utility>

#include "Vulkan.h"

namespace engine {

Buffer::Buffer(VkDevice device, VkPhysicalDevice physical, VkBufferUsageFlags usage, VkDeviceSize capacity)
    : device_(device), capacity_(capacity)
{
    if (capacity == 0)
        throw std::runtime_error("Cannot create an empty buffer");

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = capacity;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &buffer_), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);

    // Host-visible and coherent, so the CPU can write it directly without a staging copy
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = findMemoryType(physical, requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &memory_), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
    check(vkMapMemory(device_, memory_, 0, capacity_, 0, &mapped_), "vkMapMemory");
}

Buffer::Buffer(VkDevice device, VkPhysicalDevice physical, VkBufferUsageFlags usage, const void *data, VkDeviceSize size, uint32_t count)
    : Buffer(device, physical, usage, size)
{
    write(data, size, count);
}

Buffer::~Buffer()
{
    release();
}

Buffer::Buffer(Buffer &&other) noexcept
{
    *this = std::move(other);
}

Buffer &Buffer::operator=(Buffer &&other) noexcept
{
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        mapped_ = std::exchange(other.mapped_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        count_ = std::exchange(other.count_, 0);
    }
    return *this;
}

void Buffer::write(const void *data, VkDeviceSize size, uint32_t count)
{
    if (size > capacity_)
        throw std::runtime_error("Buffer write exceeds capacity");
    std::memcpy(mapped_, data, static_cast<size_t>(size));
    count_ = count;
}

void Buffer::release()
{
    if (device_ == VK_NULL_HANDLE)
        return;
    if (mapped_)
        vkUnmapMemory(device_, memory_);
    vkDestroyBuffer(device_, buffer_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    device_ = VK_NULL_HANDLE;
}

}
