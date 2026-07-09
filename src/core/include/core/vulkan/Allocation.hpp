#pragma once

#include <core/vulkan/internal/VulkanFwd.hpp>

namespace core::vk {

struct Allocation final {
    VmaAllocator allocator = VMA_NULL;
    VmaAllocation allocation = VMA_NULL;
};

} // namespace core::vk
