#include <core/vulkan/PhysicalDeviceCapsStruct.hpp>

namespace core::internal {

PhysicalDeviceCapsStruct PhysicalDeviceCapsStruct::query(PhysicalDevice const& device) {
    PhysicalDeviceCapsStruct caps{ };

    vkGetPhysicalDeviceProperties(device.handle(), &caps.properties);
    vkGetPhysicalDeviceMemoryProperties(device.handle(), &caps.memory_properties);

    if (vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceFeatures2 features2{ };
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &caps.vulkan_11_features;
        caps.vulkan_11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        caps.vulkan_11_features.pNext = &caps.vulkan_12_features;
        caps.vulkan_12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        caps.vulkan_12_features.pNext = &caps.vulkan_13_features;
        caps.vulkan_13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        caps.vulkan_13_features.pNext = &caps.vulkan_14_features;
        caps.vulkan_14_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        caps.vulkan_14_features.pNext = &caps.maintenance_4_features;
        caps.maintenance_4_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES;
        caps.maintenance_4_features.pNext = &caps.synchronization_2_features;
        caps.synchronization_2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        caps.synchronization_2_features.pNext = &caps.dynamic_rendering_features;
        caps.dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        caps.dynamic_rendering_features.pNext = &caps.mesh_shader_features;
        caps.mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        caps.mesh_shader_features.pNext = &caps.buffer_device_address_features;
        caps.buffer_device_address_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        caps.buffer_device_address_features.pNext = &caps.descriptor_indexing_features;
        caps.descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        caps.descriptor_indexing_features.pNext = &caps.timeline_semaphore_features;
        caps.timeline_semaphore_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        vkGetPhysicalDeviceFeatures2(device.handle(), &features2);

        caps.features = features2.features;
    } else {
        vkGetPhysicalDeviceFeatures(device.handle(), &caps.features);
    }

    return caps;
}

bool PhysicalDeviceCapsStruct::hasFeature(VulkanFeature const feature) const noexcept {
    switch (feature) {
        case VulkanFeature::RobustBufferAccess:
            return features.robustBufferAccess == VK_TRUE;
        case VulkanFeature::FullDrawIndexUInt32:
            return features.fullDrawIndexUint32 == VK_TRUE;
        case VulkanFeature::MultiDrawIndirect:
            return features.multiDrawIndirect == VK_TRUE;
        case VulkanFeature::DrawIndirectFirstInstance:
            return features.drawIndirectFirstInstance == VK_TRUE;
        case VulkanFeature::SamplerAnisotropy:
            return features.samplerAnisotropy == VK_TRUE;
        case VulkanFeature::FillModeNonSolid:
            return features.fillModeNonSolid == VK_TRUE;
        case VulkanFeature::GeometryShader:
            return features.geometryShader == VK_TRUE;
        case VulkanFeature::TesselationShader:
            return features.tessellationShader == VK_TRUE;
        case VulkanFeature::ShaderFloat64:
            return features.shaderFloat64 == VK_TRUE;
        case VulkanFeature::ShaderInt64:
            return features.shaderInt64 == VK_TRUE;
        case VulkanFeature::ShaderInt16:
            return features.shaderInt16 == VK_TRUE;
        case VulkanFeature::PipelineStatisticsQuery:
            return features.pipelineStatisticsQuery == VK_TRUE;

        case VulkanFeature::Multiview:
            return vulkan_11_features.multiview == VK_TRUE;
        case VulkanFeature::StorageBuffer16BitAccess:
            return vulkan_11_features.storageBuffer16BitAccess == VK_TRUE;
        case VulkanFeature::UniformAndStorageBuffer16BitAccess:
            return vulkan_11_features.uniformAndStorageBuffer16BitAccess == VK_TRUE;
        case VulkanFeature::StoragePushConstant16:
            return vulkan_11_features.storagePushConstant16 == VK_TRUE;
        case VulkanFeature::StorageInputOutput16:
            return vulkan_11_features.storageInputOutput16 == VK_TRUE;
        case VulkanFeature::VariablePointers:
            return vulkan_11_features.variablePointers == VK_TRUE;
        case VulkanFeature::VariablePointersStorageBuffer:
            return vulkan_11_features.variablePointersStorageBuffer == VK_TRUE;

        case VulkanFeature::ShaderUniformBufferArrayNonUniformIndexing:
            return descriptor_indexing_features.shaderUniformBufferArrayNonUniformIndexing == VK_TRUE;
        case VulkanFeature::ShaderSampledImageArrayNonUniformIndexing:
            return descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
        case VulkanFeature::ShaderStorageBufferArrayNonUniformIndexing:
            return descriptor_indexing_features.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE;
        case VulkanFeature::ShaderStorageImageArrayNonUniformIndexing:
            return descriptor_indexing_features.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
        case VulkanFeature::DescriptorBindingPartiallyBound:
            return descriptor_indexing_features.descriptorBindingPartiallyBound == VK_TRUE;
        case VulkanFeature::RuntimeDescriptorArray:
            return descriptor_indexing_features.runtimeDescriptorArray == VK_TRUE;
        case VulkanFeature::DescriptorBindingVariableDescriptorCount:
            return descriptor_indexing_features.descriptorBindingVariableDescriptorCount == VK_TRUE;

        case VulkanFeature::BufferDeviceAddress:
            return buffer_device_address_features.bufferDeviceAddress == VK_TRUE;
        case VulkanFeature::TimelineSemaphore:
            return timeline_semaphore_features.timelineSemaphore == VK_TRUE;

        case VulkanFeature::HostQueryReset:
            return vulkan_12_features.hostQueryReset == VK_TRUE;
        case VulkanFeature::ScalarBlockLayout:
            return vulkan_12_features.scalarBlockLayout == VK_TRUE;
        case VulkanFeature::UniformBufferStandardLayout:
            return vulkan_12_features.uniformBufferStandardLayout == VK_TRUE;
        case VulkanFeature::SeparateDepthStencilLayouts:
            return vulkan_12_features.separateDepthStencilLayouts == VK_TRUE;

        case VulkanFeature::Synchronization2:
            return vulkan_13_features.synchronization2 == VK_TRUE;
        case VulkanFeature::DynamicRendering:
            return vulkan_13_features.dynamicRendering == VK_TRUE;
        case VulkanFeature::Maintanance4:
            return vulkan_13_features.maintenance4 == VK_TRUE;
        case VulkanFeature::InlineUniformBlock:
            return vulkan_13_features.inlineUniformBlock == VK_TRUE;
        case VulkanFeature::PipelineCreationCacheControl:
            return vulkan_13_features.pipelineCreationCacheControl == VK_TRUE;
        case VulkanFeature::ShaderDemoteToHelper:
            return vulkan_13_features.shaderDemoteToHelperInvocation == VK_TRUE;
        case VulkanFeature::ShaderZeroInitializeWorkgroupMemory:
            return vulkan_13_features.shaderZeroInitializeWorkgroupMemory == VK_TRUE;
        case VulkanFeature::ComputeFullSubgroups:
            return vulkan_13_features.computeFullSubgroups == VK_TRUE;
        case VulkanFeature::SubgroupSizeControl:
            return vulkan_13_features.subgroupSizeControl == VK_TRUE;
        case VulkanFeature::ShaderIntegerDotProduct:
            return vulkan_13_features.shaderIntegerDotProduct == VK_TRUE;

        case VulkanFeature::PushDescriptor:
            return vulkan_14_features.pushDescriptor == VK_TRUE;
        case VulkanFeature::Maintanance5:
            return vulkan_14_features.maintenance5 == VK_TRUE;
        case VulkanFeature::Maintanance6:
            return vulkan_14_features.maintenance6 == VK_TRUE;
        case VulkanFeature::ShaderFloatControls2:
            return vulkan_14_features.shaderFloatControls2 == VK_TRUE;
        case VulkanFeature::ShaderSubgroupRotate:
            return vulkan_14_features.shaderSubgroupRotate == VK_TRUE;

        case VulkanFeature::TaskShader:
            return mesh_shader_features.taskShader == VK_TRUE;
        case VulkanFeature::MeshShader:
            return mesh_shader_features.meshShader == VK_TRUE;
    }
}

void PhysicalDeviceCapsStruct::setFeature(VulkanFeature const feature, bool const value) noexcept {
    VkBool32 const v = value ? VK_TRUE : VK_FALSE;
    switch (feature) {
        case VulkanFeature::RobustBufferAccess:        features.robustBufferAccess = v; break;
        case VulkanFeature::FullDrawIndexUInt32:       features.fullDrawIndexUint32 = v; break;
        case VulkanFeature::MultiDrawIndirect:         features.multiDrawIndirect = v; break;
        case VulkanFeature::DrawIndirectFirstInstance: features.drawIndirectFirstInstance = v; break;
        case VulkanFeature::SamplerAnisotropy:         features.samplerAnisotropy = v; break;
        case VulkanFeature::FillModeNonSolid:          features.fillModeNonSolid = v; break;
        case VulkanFeature::GeometryShader:            features.geometryShader = v; break;
        case VulkanFeature::TesselationShader:         features.tessellationShader = v; break;
        case VulkanFeature::ShaderFloat64:             features.shaderFloat64 = v; break;
        case VulkanFeature::ShaderInt64:               features.shaderInt64 = v; break;
        case VulkanFeature::ShaderInt16:               features.shaderInt16 = v; break;
        case VulkanFeature::PipelineStatisticsQuery:   features.pipelineStatisticsQuery = v; break;

        case VulkanFeature::Multiview:                          vulkan_11_features.multiview = v; break;
        case VulkanFeature::StorageBuffer16BitAccess:           vulkan_11_features.storageBuffer16BitAccess = v; break;
        case VulkanFeature::UniformAndStorageBuffer16BitAccess: vulkan_11_features.uniformAndStorageBuffer16BitAccess = v; break;
        case VulkanFeature::StoragePushConstant16:              vulkan_11_features.storagePushConstant16 = v; break;
        case VulkanFeature::StorageInputOutput16:               vulkan_11_features.storageInputOutput16 = v; break;
        case VulkanFeature::VariablePointers:                   vulkan_11_features.variablePointers = v; break;
        case VulkanFeature::VariablePointersStorageBuffer:      vulkan_11_features.variablePointersStorageBuffer = v; break;

        case VulkanFeature::ShaderUniformBufferArrayNonUniformIndexing:
            descriptor_indexing_features.shaderUniformBufferArrayNonUniformIndexing = v;
            break;
        case VulkanFeature::ShaderSampledImageArrayNonUniformIndexing:
            descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing = v;
            break;
        case VulkanFeature::ShaderStorageBufferArrayNonUniformIndexing:
            descriptor_indexing_features.shaderStorageBufferArrayNonUniformIndexing = v;
            break;
        case VulkanFeature::ShaderStorageImageArrayNonUniformIndexing:
            descriptor_indexing_features.shaderStorageImageArrayNonUniformIndexing = v;
            break;
        case VulkanFeature::DescriptorBindingPartiallyBound:
            descriptor_indexing_features.descriptorBindingPartiallyBound = v;
            break;
        case VulkanFeature::RuntimeDescriptorArray:
            descriptor_indexing_features.runtimeDescriptorArray = v;
            break;
        case VulkanFeature::DescriptorBindingVariableDescriptorCount:
            descriptor_indexing_features.descriptorBindingVariableDescriptorCount = v;
            break;

        case VulkanFeature::BufferDeviceAddress: buffer_device_address_features.bufferDeviceAddress = v; break;
        case VulkanFeature::TimelineSemaphore:   timeline_semaphore_features.timelineSemaphore = v; break;

        case VulkanFeature::HostQueryReset:              vulkan_12_features.hostQueryReset = v; break;
        case VulkanFeature::ScalarBlockLayout:           vulkan_12_features.scalarBlockLayout = v; break;
        case VulkanFeature::UniformBufferStandardLayout: vulkan_12_features.uniformBufferStandardLayout = v; break;
        case VulkanFeature::SeparateDepthStencilLayouts: vulkan_12_features.separateDepthStencilLayouts = v; break;

        case VulkanFeature::Synchronization2:                    vulkan_13_features.synchronization2 = v; break;
        case VulkanFeature::DynamicRendering:                    vulkan_13_features.dynamicRendering = v; break;
        case VulkanFeature::Maintanance4:                        vulkan_13_features.maintenance4 = v; break;
        case VulkanFeature::InlineUniformBlock:                  vulkan_13_features.inlineUniformBlock = v; break;
        case VulkanFeature::PipelineCreationCacheControl:        vulkan_13_features.pipelineCreationCacheControl = v; break;
        case VulkanFeature::ShaderDemoteToHelper:                vulkan_13_features.shaderDemoteToHelperInvocation = v; break;
        case VulkanFeature::ShaderZeroInitializeWorkgroupMemory: vulkan_13_features.shaderZeroInitializeWorkgroupMemory = v; break;
        case VulkanFeature::ComputeFullSubgroups:                vulkan_13_features.computeFullSubgroups = v; break;
        case VulkanFeature::SubgroupSizeControl:                 vulkan_13_features.subgroupSizeControl = v; break;
        case VulkanFeature::ShaderIntegerDotProduct:             vulkan_13_features.shaderIntegerDotProduct = v; break;

        case VulkanFeature::PushDescriptor:       vulkan_14_features.pushDescriptor = v; break;
        case VulkanFeature::Maintanance5:         vulkan_14_features.maintenance5 = v; break;
        case VulkanFeature::Maintanance6:         vulkan_14_features.maintenance6 = v; break;
        case VulkanFeature::ShaderFloatControls2: vulkan_14_features.shaderFloatControls2 = v; break;
        case VulkanFeature::ShaderSubgroupRotate: vulkan_14_features.shaderSubgroupRotate = v; break;

        case VulkanFeature::TaskShader: mesh_shader_features.taskShader = v; break;
        case VulkanFeature::MeshShader: mesh_shader_features.meshShader = v; break;
    }
}

std::vector<MemoryHeap> PhysicalDeviceCapsStruct::memoryHeaps() const {
    std::span memory_types_span(memory_properties.memoryTypes, memory_properties.memoryTypeCount);
    std::vector<MemoryHeap> result;
    result.reserve(memory_properties.memoryTypeCount);
    for (VkMemoryType const& memory : memory_types_span) {
        result.push_back(MemoryHeap{
            .properties = MemoryPropertyBits{ memory.propertyFlags },
            .heap_index = memory.heapIndex,
        });
    }
    return result;
}

VulkanFeatures PhysicalDeviceCapsStruct::getFeatures() const noexcept {
    VulkanFeatures result{ };
    for (VulkanFeature const feature : valuesOf<VulkanFeature>()) {
        if (hasFeature(feature)) {
            result[feature] = true;
        }
    }
    return result;
}

} // namespace core::internal
