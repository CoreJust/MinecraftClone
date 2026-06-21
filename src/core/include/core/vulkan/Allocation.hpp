#pragma once

#include <core/vulkan/VulkanFwd.hpp>

namespace core {

struct Allocation final {
    VmaAllocator allocator = VMA_NULL;
    VmaAllocation allocation = VMA_NULL;
};

} // namespace core
