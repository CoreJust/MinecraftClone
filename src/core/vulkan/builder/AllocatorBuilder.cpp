#include <core/vulkan/builder/AllocatorBuilder.hpp>

#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/VulkanVersion.hpp>
#include <core/vulkan/internal/VMA.hpp>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::AllocatorCreationErrorKind);

namespace core::vk {
namespace {

[[nodiscard]]
VmaAllocatorCreateFlags getVmaAllocatorFlags(
    VulkanCaps const& caps,
    bool const externally_synchronized
) noexcept {
    VmaAllocatorCreateFlags flags = 0;

    if (externally_synchronized) {
        flags |= VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
    }
    if (caps.has(VulkanExtension::DedicatedAllocation)) {
        flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    }
    if (caps.has(VulkanExtension::BindMemory2)) {
        flags |= VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT;
    }
    if (caps.has(VulkanExtension::MemoryBudget)) {
        flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    if (caps.has(VulkanExtension::DeviceCoherentMemory)) {
        flags |= VMA_ALLOCATOR_CREATE_AMD_DEVICE_COHERENT_MEMORY_BIT;
    }
    if (caps.has(VulkanExtension::BufferDeviceAddress)
        || caps.has(VulkanFeature::BufferDeviceAddress)
    ) {
        flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    if (caps.has(VulkanExtension::MemoryPriority)) {
        flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }
    if (caps.has(VulkanExtension::Maintenance4)) {
        flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    }
    if (caps.has(VulkanExtension::Maintenance5)) {
        flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
    }

    return flags;
}

} // namespace

Allocator AllocatorBuilder::build(VulkanCaps const& caps) const {
    VmaAllocatorCreateInfo create_info{
        .flags = getVmaAllocatorFlags(caps, m_externally_synchronized),
        .physicalDevice = m_physical_device.handle(),
        .device = m_device.handle(),
        .preferredLargeHeapBlockSize = m_preferred_large_heap_block_size,
        .instance = m_instance.handle(),
        .vulkanApiVersion = versionToVk(caps.deviceVersion()),
    };

    VmaVulkanFunctions vulkan_functions{ };
    if (!VK_CHECK(vmaImportVulkanFunctionsFromVolk(&create_info, &vulkan_functions))) {
        throw AllocatorCreationError(AllocatorCreationError::FailedToLoadVulkanFunctions);
    }
    create_info.pVulkanFunctions = &vulkan_functions;

    VmaAllocator allocator = VMA_NULL;
    if (!VK_CHECK(vmaCreateAllocator(&create_info, &allocator))) {
        throw AllocatorCreationError(AllocatorCreationError::FailedToCreateVmaAllocator);
    }

    return Allocator(allocator);
}

} // namespace core::vk
