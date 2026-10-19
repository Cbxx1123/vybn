#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "Texture.h"

namespace engine {

struct DescriptorBinding {
    VkDescriptorType type;     // e.g. VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    VkShaderStageFlags stages; // binding number is its index in the layout
};

// The shape of a descriptor set: what is bound at each binding. Pass it to a Pipeline
// through PipelineOptions and allocate sets of it from a DescriptorPool.
// Must be destroyed before its Renderer.
class DescriptorSetLayout {
public:
    DescriptorSetLayout(VkDevice device, const std::vector<DescriptorBinding> &bindings);
    ~DescriptorSetLayout();

    DescriptorSetLayout(const DescriptorSetLayout &) = delete;
    DescriptorSetLayout &operator=(const DescriptorSetLayout &) = delete;

    VkDescriptorSetLayout handle() const { return layout_; }

private:
    VkDevice device_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

// A handle to a set allocated from a DescriptorPool; it is freed with the pool.
// Only rewrite it while no frame in flight uses it (in practice: once, right after allocating).
class DescriptorSet {
public:
    DescriptorSet() = default;
    DescriptorSet(VkDevice device, VkDescriptorSet set) : device_(device), set_(set) {}

    // Points `binding` (a combined image sampler) at `texture`
    void writeTexture(uint32_t binding, const Texture &texture);
    // Points `binding` at a live texture (GENERAL layout)
    void writeTexture(uint32_t binding, const LiveTexture &texture);

    VkDescriptorSet handle() const { return set_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

// Room for `maxSets` sets holding up to `maxTextures` combined image samplers between them.
// Must be destroyed before its Renderer, and outlives the sets allocated from it.
class DescriptorPool {
public:
    DescriptorPool(VkDevice device, uint32_t maxSets, uint32_t maxTextures);
    ~DescriptorPool();

    DescriptorPool(const DescriptorPool &) = delete;
    DescriptorPool &operator=(const DescriptorPool &) = delete;

    DescriptorSet allocate(const DescriptorSetLayout &layout);

private:
    VkDevice device_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

}
