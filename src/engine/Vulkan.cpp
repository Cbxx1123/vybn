#include "Vulkan.h"

#include <stdexcept>
#include <string>

namespace engine {

void check(VkResult result, const char *what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        bool typeAllowed = typeFilter & (1u << i);
        bool hasProperties = (memory.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeAllowed && hasProperties)
            return i;
    }
    throw std::runtime_error("No suitable GPU memory type");
}

}
