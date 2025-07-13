#pragma once
#include <vulkan/vulkan.h>
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::internal {
    class Vulkan;

    class ShaderModule final {
        VkShaderModule m_shaderModule = VK_NULL_HANDLE;
        Vulkan& m_vulkan;

    public:
        ShaderModule(Vulkan& vulkan, char const* const path);
        ~ShaderModule();

        PURE VkPipelineShaderStageCreateInfo makeCreationInfo(VkShaderStageFlagBits stage) const;
        PURE VkShaderModule get() const noexcept { return m_shaderModule; }
    };
} // namespace graphics::vulkan::internal
