#pragma once
#include <vulkan/vulkan.hpp>
#include <Graphics/Vulkan/Pipeline/Attribute.hpp>

namespace graphics::vulkan::internal {
    VkFormat attributeFormat(pipeline::Attribute attr);
} // namespace graphics::vulkan::internal
