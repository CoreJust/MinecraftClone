#pragma once

#include <core/meta/Enum.hpp>

#include <bitset>
#include <string>

namespace core::vk {

struct VulkanPhysicalDeviceFeatures;

enum class VulkanFeature {
    // VkPhysicalDeviceFeatures
    RobustBufferAccess,
    FullDrawIndexUInt32,
    MultiDrawIndirect,
    DrawIndirectFirstInstance,
    SamplerAnisotropy,
    FillModeNonSolid,
    GeometryShader,
    TesselationShader,
    ShaderFloat64,
    ShaderInt64,
    ShaderInt16,
    PipelineStatisticsQuery,

    // VkPhysicalDeviceVulkan11Features
    Multiview,
    StorageBuffer16BitAccess,
    UniformAndStorageBuffer16BitAccess,
    StoragePushConstant16,
    StorageInputOutput16,
    VariablePointers,
    VariablePointersStorageBuffer,

    // VkPhysicalDeviceVulkan12Features
    ShaderUniformBufferArrayNonUniformIndexing,
    ShaderSampledImageArrayNonUniformIndexing,
    ShaderStorageBufferArrayNonUniformIndexing,
    ShaderStorageImageArrayNonUniformIndexing,
    DescriptorBindingPartiallyBound,
    RuntimeDescriptorArray,
    DescriptorBindingVariableDescriptorCount,
    BufferDeviceAddress,
    TimelineSemaphore,
    HostQueryReset,
    ScalarBlockLayout,
    UniformBufferStandardLayout,
    SeparateDepthStencilLayouts,

    // VkPhysicalDeviceVulkan13Features
    Synchronization2,
    DynamicRendering,
    Maintanance4,
    InlineUniformBlock,
    PipelineCreationCacheControl,
    ShaderDemoteToHelper,
    ShaderZeroInitializeWorkgroupMemory,
    ComputeFullSubgroups,
    SubgroupSizeControl,
    ShaderIntegerDotProduct,

    // VkPhysicalDeviceVulkan14Features
    PushDescriptor,
    Maintanance5,
    Maintanance6,
    ShaderFloatControls2,
    ShaderSubgroupRotate,

    // VkPhysicalDeviceMeshShaderFeaturesEXT
    TaskShader,
    MeshShader,

    Count,
};

struct VulkanFeatures final {
    std::bitset<countOf<VulkanFeature>()> data;

    auto operator[](VulkanFeature const feature) noexcept {
        return data[indexOf(feature)];
    }

    auto operator[](VulkanFeature const feature) const noexcept {
        return data[indexOf(feature)];
    }

    [[nodiscard]]
    std::string toString(std::string_view const indent = "") const;
};

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::VulkanFeature);
