#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "Descriptors.h"
#include "Shader.h"

namespace engine {

struct VertexAttribute {
    VkFormat format;
    uint32_t offset; // byte offset inside one vertex; attribute location is its index in the layout
};

struct VertexLayout {
    uint32_t stride; // sizeof(Vertex)
    std::vector<VertexAttribute> attributes;
    // VK_VERTEX_INPUT_RATE_INSTANCE advances once per instance (see Renderer::drawInstanced)
    VkVertexInputRate inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
};

struct PipelineOptions {
    bool alphaBlending = false;
    uint32_t pushConstantSize = 0; // bytes visible to vertex and fragment stages
    // Set 0 of the pipeline layout, for shaders that read descriptors (e.g. a sampled texture).
    // Only read while the pipeline is created; bind sets with Renderer::bindDescriptorSet.
    const DescriptorSetLayout *descriptorSetLayout = nullptr;
};

// A graphics pipeline: vertex and fragment shaders, a vertex layout, drawing triangles.
// Viewport and scissor are dynamic, so one pipeline works at any window size.
class Pipeline {
public:
    Pipeline(VkDevice device, VkRenderPass renderPass, const Shader &vertex, const Shader &fragment,
             const VertexLayout &layout, const PipelineOptions &options);
    ~Pipeline();

    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkDevice device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}
