#include "Allocator.hpp"
#include <vk_mem_alloc.h>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "../Check.hpp"
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    Allocator::Allocator(Vulkan& vulkan) {
        VmaAllocatorCreateInfo createInfo { };
        createInfo.physicalDevice = vulkan.physicalDevice().get();
        createInfo.device         = vulkan.device().get();

        if (!VK_CHECK(vmaCreateAllocator(&createInfo, &m_allocator))) {
            core::fatal("Failed to create VMA allocator!");
            throw VulkanException { };
        }
    }

    Allocator::~Allocator() {
        if (m_allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(m_allocator);
            m_allocator = VK_NULL_HANDLE;
        }
    }
} // namespace graphics::vulkan::internal
