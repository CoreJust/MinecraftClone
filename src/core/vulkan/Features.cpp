#include <core/vulkan/Features.hpp>

#include <core/common/IterEnum.hpp>

#include <fmt/core.h>

namespace core {

std::string to_string(VulkanFeature const feature) {
    switch (feature) {
        // VkPhysicalDeviceFeatures
        case VulkanFeature::RobustBufferAccess:            return "RobustBufferAccess";
        case VulkanFeature::FullDrawIndexUInt32:           return "FullDrawIndexUInt32";
        case VulkanFeature::MultiDrawIndirect:             return "MultiDrawIndirect";
        case VulkanFeature::DrawIndirectFirstInstance:     return "DrawIndirectFirstInstance";
        case VulkanFeature::SamplerAnisotropy:             return "SamplerAnisotropy";
        case VulkanFeature::FillModeNonSolid:              return "FillModeNonSolid";
        case VulkanFeature::GeometryShader:                return "GeometryShader";
        case VulkanFeature::TesselationShader:             return "TesselationShader";
        case VulkanFeature::ShaderFloat64:                 return "ShaderFloat64";
        case VulkanFeature::ShaderInt64:                   return "ShaderInt64";
        case VulkanFeature::ShaderInt16:                   return "ShaderInt16";
        case VulkanFeature::PipelineStatisticsQuery:       return "PipelineStatisticsQuery";

        // VkPhysicalDeviceVulkan11Features
        case VulkanFeature::Multiview:                     return "Multiview";
        case VulkanFeature::StorageBuffer16BitAccess:      return "StorageBuffer16BitAccess";
        case VulkanFeature::UniformAndStorageBuffer16BitAccess: return "UniformAndStorageBuffer16BitAccess";
        case VulkanFeature::StoragePushConstant16:         return "StoragePushConstant16";
        case VulkanFeature::StorageInputOutput16:          return "StorageInputOutput16";
        case VulkanFeature::VariablePointers:              return "VariablePointers";
        case VulkanFeature::VariablePointersStorageBuffer: return "VariablePointersStorageBuffer";

        // VkPhysicalDeviceVulkan12Features
        case VulkanFeature::ShaderUniformBufferArrayNonUniformIndexing: return "ShaderUniformBufferArrayNonUniformIndexing";
        case VulkanFeature::ShaderSampledImageArrayNonUniformIndexing: return "ShaderSampledImageArrayNonUniformIndexing";
        case VulkanFeature::ShaderStorageBufferArrayNonUniformIndexing: return "ShaderStorageBufferArrayNonUniformIndexing";
        case VulkanFeature::ShaderStorageImageArrayNonUniformIndexing: return "ShaderStorageImageArrayNonUniformIndexing";
        case VulkanFeature::DescriptorBindingPartiallyBound: return "DescriptorBindingPartiallyBound";
        case VulkanFeature::RuntimeDescriptorArray:        return "RuntimeDescriptorArray";
        case VulkanFeature::DescriptorBindingVariableDescriptorCount: return "DescriptorBindingVariableDescriptorCount";
        case VulkanFeature::BufferDeviceAddress:           return "BufferDeviceAddress";
        case VulkanFeature::TimelineSemaphore:             return "TimelineSemaphore";
        case VulkanFeature::HostQueryReset:                return "HostQueryReset";
        case VulkanFeature::ScalarBlockLayout:             return "ScalarBlockLayout";
        case VulkanFeature::UniformBufferStandardLayout:   return "UniformBufferStandardLayout";
        case VulkanFeature::SeparateDepthStencilLayouts:   return "SeparateDepthStencilLayouts";

        // VkPhysicalDeviceVulkan13Features
        case VulkanFeature::Synchronization2:              return "Synchronization2";
        case VulkanFeature::DynamicRendering:              return "DynamicRendering";
        case VulkanFeature::Maintanance4:                  return "Maintanance4";
        case VulkanFeature::InlineUniformBlock:            return "InlineUniformBlock";
        case VulkanFeature::PipelineCreationCacheControl:  return "PipelineCreationCacheControl";
        case VulkanFeature::ShaderDemoteToHelper:          return "ShaderDemoteToHelper";
        case VulkanFeature::ShaderZeroInitializeWorkgroupMemory: return "ShaderZeroInitializeWorkgroupMemory";
        case VulkanFeature::ComputeFullSubgroups:          return "ComputeFullSubgroups";
        case VulkanFeature::SubgroupSizeControl:           return "SubgroupSizeControl";
        case VulkanFeature::ShaderIntegerDotProduct:       return "ShaderIntegerDotProduct";

        // VkPhysicalDeviceVulkan14Features
        case VulkanFeature::PushDescriptor:                return "PushDescriptor";
        case VulkanFeature::Maintanance5:                  return "Maintanance5";
        case VulkanFeature::Maintanance6:                  return "Maintanance6";
        case VulkanFeature::ShaderFloatControls2:          return "ShaderFloatControls2";
        case VulkanFeature::ShaderSubgroupRotate:          return "ShaderSubgroupRotate";

        // VkPhysicalDeviceMeshShaderFeaturesEXT
        case VulkanFeature::TaskShader:                    return "TaskShader";
        case VulkanFeature::MeshShader:                    return "MeshShader";
    }
}

std::string VulkanFeatures::toString(std::string_view const indent) const {
    std::string features_message;
    for (VulkanFeature const feature : iterEnum<VulkanFeature>()) {
        if (this->operator[](feature)) {
            features_message += fmt::format("{}{:40}\n", indent, to_string(feature));
        }
    }
    return features_message;
}

} // namespace core
