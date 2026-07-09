#pragma once

#include <core/vulkan/Allocator.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Instance.hpp>
#include <core/vulkan/PhysicalDevice.hpp>

CORE_VK_ERROR_WITH_KINDS(AllocatorCreationError, VulkanInitializationError,
    FailedToLoadVulkanFunctions,
    FailedToCreateVmaAllocator);

namespace core::vk {

class AllocatorBuilder final {
public:
    AllocatorBuilder(
        Device const& device,
        PhysicalDevice const& physical_device,
        Instance const& instance)
        : m_device(device)
        , m_physical_device(physical_device)
        , m_instance(instance)
    { }

    // 0 for default
    template<typename Self>
    auto&& preferLargeHeapBlockSize(this Self&& self, VkDeviceSize const value) {
        self.m_preferred_large_heap_block_size = value;
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& synchronizeExternally(this Self&& self, bool const value = true) {
        self.m_externally_synchronized = value;
        return std::forward<Self>(self);
    }

    [[nodiscard]]
    Allocator build(VulkanCaps const& caps) const;
private:
    Device const& m_device;
    PhysicalDevice const& m_physical_device;
    Instance const& m_instance;

    VkDeviceSize m_preferred_large_heap_block_size = 0;
    bool m_externally_synchronized = false;
};

} // namespace core::vk
