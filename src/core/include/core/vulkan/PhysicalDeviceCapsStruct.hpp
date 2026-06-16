#pragma once

#include <core/vulkan/Features.hpp>
#include <core/vulkan/MemoryProperty.hpp>
#include <core/vulkan/PhysicalDevice.hpp>
#include <core/vulkan/PhysicalDeviceProperties.hpp>
#include <core/vulkan/PhysicalDeviceType.hpp>
#include <core/vulkan/VulkanVersion.hpp>

#include <volk.h>

#include <optional>

namespace core::internal {

struct PhysicalDeviceCapsStruct final {
    VkPhysicalDeviceProperties properties{ };
    VkPhysicalDeviceMemoryProperties memory_properties{ };
    VkPhysicalDeviceFeatures features{ };
    VkPhysicalDeviceVulkan11Features vulkan_11_features{ };
    VkPhysicalDeviceVulkan12Features vulkan_12_features{ };
    VkPhysicalDeviceVulkan13Features vulkan_13_features{ };
    VkPhysicalDeviceVulkan14Features vulkan_14_features{ };
    VkPhysicalDeviceMaintenance4Features maintenance_4_features{ };
    VkPhysicalDeviceSynchronization2Features synchronization_2_features{ };
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features{ };
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features{ };
    VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address_features{ };
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_features{ };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore_features{ };

    [[nodiscard]]
    static PhysicalDeviceCapsStruct query(PhysicalDevice const& device);

    [[nodiscard]]
    bool hasFeature(VulkanFeature const feature) const noexcept;
    void setFeature(VulkanFeature const feature, bool const value = true) noexcept;

    [[nodiscard]]
    PhysicalDeviceType type() const noexcept {
        return static_cast<PhysicalDeviceType>(properties.deviceType);
    }

    void setType(PhysicalDeviceType const type) noexcept {
        properties.deviceType = static_cast<VkPhysicalDeviceType>(type);
    }

    [[nodiscard]]
    Version apiVersion() const noexcept {
        return vkToVersion(properties.apiVersion);
    }

    [[nodiscard]]
    void setApiVersion(Version const version) noexcept {
        properties.apiVersion = versionToVk(version);
    }

    [[nodiscard]]
    std::optional<uint32_t> findHeapWith(MemoryPropertyBits const bits) const noexcept {
        std::span memory_types_span(memory_properties.memoryTypes, memory_properties.memoryTypeCount);
        for (VkMemoryType const& memory : memory_types_span) {
            if ((memory.propertyFlags & bits.value) == bits.value) {
                return memory.heapIndex;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    bool hasHeapWith(MemoryPropertyBits const bits) const noexcept {
        return findHeapWith(bits).has_value();
    }

    [[nodiscard]]
    std::vector<MemoryHeap> memoryHeaps() const;

    [[nodiscard]]
    VulkanFeatures getFeatures() const noexcept;

    [[nodiscard]]
    PhysicalDeviceProperties getProperties() const noexcept {
        return PhysicalDeviceProperties{
            .sparse = std::bit_cast<PhysicalDeviceSparseProperties>(properties.sparseProperties),
            .limits = std::bit_cast<PhysicalDeviceLimits>(properties.limits),
        };
    }

    [[nodiscard]]
    std::string_view deviceName() const noexcept { return properties.deviceName; }

    [[nodiscard]]
    PhysicalDeviceType deviceType() const noexcept {
        return static_cast<PhysicalDeviceType>(properties.deviceType);
    }
};

} // namespace core::internal
