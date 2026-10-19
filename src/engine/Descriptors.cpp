#include "Descriptors.h"

#include "Vulkan.h"

namespace engine {

// DescriptorSetLayout

DescriptorSetLayout::DescriptorSetLayout(VkDevice device, const std::vector<DescriptorBinding> &bindings)
    : device_(device)
{
    std::vector<VkDescriptorSetLayoutBinding> vkBindings(bindings.size());
    for (uint32_t i = 0; i < vkBindings.size(); ++i) {
        vkBindings[i].binding = i;
        vkBindings[i].descriptorType = bindings[i].type;
        vkBindings[i].descriptorCount = 1;
        vkBindings[i].stageFlags = bindings[i].stages;
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(vkBindings.size());
    info.pBindings = vkBindings.data();
    check(vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout_), "vkCreateDescriptorSetLayout");
}

DescriptorSetLayout::~DescriptorSetLayout()
{
    vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
}

// DescriptorSet

void DescriptorSet::writeTexture(uint32_t binding, const Texture &texture)
{
    VkDescriptorImageInfo image{};
    image.sampler = texture.sampler();
    image.imageView = texture.view();
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void DescriptorSet::writeTexture(uint32_t binding, const LiveTexture &texture)
{
    VkDescriptorImageInfo image{};
    image.sampler = texture.sampler();
    image.imageView = texture.view();
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

// DescriptorPool

DescriptorPool::DescriptorPool(VkDevice device, uint32_t maxSets, uint32_t maxTextures)
    : device_(device)
{
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = maxTextures;

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = maxSets;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device_, &info, nullptr, &pool_), "vkCreateDescriptorPool");
}

DescriptorPool::~DescriptorPool()
{
    vkDestroyDescriptorPool(device_, pool_, nullptr);
}

DescriptorSet DescriptorPool::allocate(const DescriptorSetLayout &layout)
{
    VkDescriptorSetLayout handle = layout.handle();
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &handle;

    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(device_, &info, &set), "vkAllocateDescriptorSets");
    return DescriptorSet(device_, set);
}

}
