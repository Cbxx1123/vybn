#pragma once

#include <string>

#include <vulkan/vulkan.h>

namespace engine {

enum class ShaderStage { Vertex, Fragment };

// A compiled SPIR-V shader module. Destroyed with the object; must be destroyed before its Renderer.
class Shader {
public:
    Shader(VkDevice device, const std::string &spirvPath, ShaderStage stage);
    ~Shader();

    Shader(const Shader &) = delete;
    Shader &operator=(const Shader &) = delete;

    VkShaderModule module() const { return module_; }
    VkShaderStageFlagBits vkStage() const
    {
        return stage_ == ShaderStage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }

private:
    VkDevice device_;
    VkShaderModule module_ = VK_NULL_HANDLE;
    ShaderStage stage_;
};

}
