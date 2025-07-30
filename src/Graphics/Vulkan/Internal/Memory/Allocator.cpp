#include "Allocator.hpp"
#include <vk_mem_alloc.h>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include <Graphics/Vulkan/Version.hpp>
#include "../Check.hpp"
#include "../Vulkan/Extensions.hpp"
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
namespace {
    VmaAllocatorCreateFlags makeAllocatorFlags() noexcept {
        return 
               VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT
            | (VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT   * hasExtension(VulkanExtension::DedicatedAllocation))
            | (VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT           * hasExtension(VulkanExtension::BindMemory2))
            | (VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT          * hasExtension(VulkanExtension::MemoryBudget))
            | (VMA_ALLOCATOR_CREATE_AMD_DEVICE_COHERENT_MEMORY_BIT * hasExtension(VulkanExtension::DeviceCoherentMemory))
            | (VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT      * hasExtension(VulkanExtension::BufferDeviceAddress))
            | (VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT        * hasExtension(VulkanExtension::MemoryPriority))
            | (VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT           * hasExtension(VulkanExtension::Maintenance4))
            | (VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT           * hasExtension(VulkanExtension::Maintenance5));
    }
} // namespace


    Allocator::Allocator(Vulkan& vulkan) {
        VmaAllocatorCreateInfo createInfo { };
        createInfo.flags            = makeAllocatorFlags();
        createInfo.instance         = vulkan.instance()      .get();
        createInfo.vulkanApiVersion = getDeviceVkVersion()   .asVk();
        createInfo.physicalDevice   = vulkan.physicalDevice().get();
        createInfo.device           = vulkan.device()        .get();

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
