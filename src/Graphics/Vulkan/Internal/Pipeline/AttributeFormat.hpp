#pragma once
#include <vulkan/vulkan.hpp>
#include <Core/Container/DynArray.hpp>
#include <Graphics/Vulkan/Pipeline/Attribute.hpp>
#include "ShaderStageBit.hpp"

namespace graphics::vulkan::internal {
    struct VulkanVertexLayoutDescription final {
        core::DynArray<VkVertexInputAttributeDescription> attrDescriptions;
        VkVertexInputBindingDescription bindingDescription;
    };

    VkFormat attributeFormat(pipeline::Attribute attr);
    u32 attributeSize(pipeline::Attribute attr);
    u32 attributeLocationSize(pipeline::Attribute attr);
    VulkanVertexLayoutDescription makeVertexLayoutDescription(core::ArrayView<pipeline::Attribute const> attrs);
    VkPushConstantRange makePushConstantsDescription(ShaderStageBit stage, core::ArrayView<pipeline::Attribute const> attrs);
} // namespace graphics::vulkan::internal
