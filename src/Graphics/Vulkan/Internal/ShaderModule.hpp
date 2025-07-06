#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class ShaderModule final {
        VkShaderModule m_shaderModule = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE; // Device used to create the shader

    public:
        ShaderModule(VkDevice device, char const* const path);
        ~ShaderModule();

        PURE VkPipelineShaderStageCreateInfo makeCreationInfo(VkShaderStageFlagBits stage) const;
        PURE VkShaderModule get() const noexcept { return m_shaderModule; }
    };
} // namespace graphics::vulkan::internal
