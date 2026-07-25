#pragma once

#include <core/vulkan/Device.hpp>
#include <core/vulkan/SpirV.hpp>

namespace core::vk {

class RawShaderModule : public VulkanResourceBase<VkShaderModule> {
    CORE_VK_RESOURCE_CONTEXT(RawShaderModule,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(
        Device const& device,
        SpirV const& spir_v
    );
};

using ShaderModule = VulkanRaii<RawShaderModule>;

} // namespace core::vk
