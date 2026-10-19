#include "Shader.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "Vulkan.h"

namespace engine {

Shader::Shader(VkDevice device, const std::string &spirvPath, ShaderStage stage)
    : device_(device), stage_(stage)
{
    std::ifstream file(spirvPath, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Failed to open shader " + spirvPath);

    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V file " + spirvPath);

    // SPIR-V is a stream of 32-bit words
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), size);

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(size);
    info.pCode = code.data();
    check(vkCreateShaderModule(device_, &info, nullptr, &module_), "vkCreateShaderModule");
}

Shader::~Shader()
{
    vkDestroyShaderModule(device_, module_, nullptr);
}

}
