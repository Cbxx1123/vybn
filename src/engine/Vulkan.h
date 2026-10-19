#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace engine {

// Throws std::runtime_error with `what` and the result code if `result` is not VK_SUCCESS.
void check(VkResult result, const char *what);

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeFilter, VkMemoryPropertyFlags properties);

}
