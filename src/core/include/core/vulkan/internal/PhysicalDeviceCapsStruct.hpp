#pragma once

#include <core/vulkan/Features.hpp>
#include <core/vulkan/MemoryProperty.hpp>
#include <core/vulkan/PhysicalDevice.hpp>
#include <core/vulkan/PhysicalDeviceProperties.hpp>
#include <core/vulkan/PhysicalDeviceType.hpp>
#include <core/vulkan/VulkanVersion.hpp>

#include <volk.h>

#include <optional>
#include <span>

namespace core::internal {

struct PhysicalDeviceCapsStruct final {
    VkPhysicalDeviceProperties properties{ };
    VkPhysicalDeviceMemoryProperties memory_properties{ };
    VkPhysicalDeviceFeatures features{ };

    VkPhysicalDeviceVulkan11Features vulkan_11_features{ };
    VkPhysicalDeviceVulkan12Features vulkan_12_features{ };
    VkPhysicalDeviceVulkan13Features vulkan_13_features{ };
    VkPhysicalDeviceVulkan14Features vulkan_14_features{ };

    // 1.1 promotions
    VkPhysicalDevice16BitStorageFeatures              feat_16bit_storage{ };
    VkPhysicalDeviceMultiviewFeatures                 feat_multiview{ };
    VkPhysicalDeviceVariablePointersFeatures          feat_variable_pointers{ };

    // 1.2 promotions
    VkPhysicalDevice8BitStorageFeatures               feat_8bit_storage{ };
    VkPhysicalDeviceShaderFloat16Int8Features         feat_shader_float16_int8{ };
    VkPhysicalDeviceDescriptorIndexingFeatures        feat_descriptor_indexing{ };
    VkPhysicalDeviceBufferDeviceAddressFeatures       feat_buffer_device_address{ };
    VkPhysicalDeviceHostQueryResetFeatures            feat_host_query_reset{ };
    VkPhysicalDeviceTimelineSemaphoreFeatures         feat_timeline_semaphore{ };
    VkPhysicalDeviceVulkanMemoryModelFeatures         feat_vulkan_memory_model{ };
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures feat_subgroup_extended_types{ };
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures feat_separate_depth_stencil_layouts{ };
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures feat_uniform_buffer_standard_layout{ };
    VkPhysicalDeviceShaderAtomicInt64Features         feat_shader_atomic_int64{ };
    VkPhysicalDeviceScalarBlockLayoutFeatures         feat_scalar_block_layout{ };
    VkPhysicalDeviceImagelessFramebufferFeatures      feat_imageless_framebuffer{ };

    // 1.3 promotions
    VkPhysicalDeviceMaintenance4Features              feat_maintenance4{ };
    VkPhysicalDeviceSynchronization2Features          feat_synchronization2{ };
    VkPhysicalDeviceDynamicRenderingFeatures          feat_dynamic_rendering{ };

    // Not promoted
    VkPhysicalDeviceMeshShaderFeaturesEXT             mesh_shader_features{ };

    [[nodiscard]]
    void* chained(Version instance_version) noexcept;
    [[nodiscard]]
    static PhysicalDeviceCapsStruct query(PhysicalDevice const& device, Version instance_version);

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
