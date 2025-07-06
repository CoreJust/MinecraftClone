#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Device;
    class Swapchain;

    class Pipeline final {
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE; // Device used to create the pipeline

    public:
        Pipeline(Device& device, Swapchain& swapchain, char const* const vertexShaderPath, char const* const fragmentShaderPath);
        ~Pipeline();
    };
} // namespace graphics::vulkan::internal
