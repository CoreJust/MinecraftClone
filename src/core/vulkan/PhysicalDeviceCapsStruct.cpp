#include <core/vulkan/PhysicalDeviceCapsStruct.hpp>

#include <core/common/Assert.hpp>

namespace core::internal {

namespace {

template<typename FeatureSet>
struct FeatureSetTraits final{ };

#define DECLARE_FEATURE_SET_CORE(version)                                         \
    template<>                                                                    \
    struct FeatureSetTraits<VkPhysicalDeviceVulkan1##version##Features> final {   \
        static constexpr bool CORE = true;                                        \
        static constexpr bool PROMOTED = false;                                   \
        static constexpr uint32_t INTRODUCTION_VERSION = version;                 \
        static constexpr auto CoreField = &PhysicalDeviceCapsStruct::vulkan_1##version##_features; \
        using CoreVersion = VkPhysicalDeviceVulkan1##version##Features;           \
    };

#define DECLARE_FEATURE_SET_PROMOTION(feature_set, promotion_version)             \
    template<>                                                                    \
    struct FeatureSetTraits<VkPhysicalDevice##feature_set##Features> final {      \
        static constexpr bool CORE = false;                                       \
        static constexpr bool PROMOTED = true;                                    \
        static constexpr uint32_t PROMOTION_VERSION = promotion_version;          \
        static constexpr auto CoreField = &PhysicalDeviceCapsStruct::vulkan_1##promotion_version##_features; \
        using CoreVersion = VkPhysicalDeviceVulkan1##promotion_version##Features; \
    };

DECLARE_FEATURE_SET_CORE(1)
DECLARE_FEATURE_SET_CORE(2)
DECLARE_FEATURE_SET_CORE(3)
DECLARE_FEATURE_SET_CORE(4)

DECLARE_FEATURE_SET_PROMOTION(16BitStorage, 1)
DECLARE_FEATURE_SET_PROMOTION(Multiview, 1)
DECLARE_FEATURE_SET_PROMOTION(VariablePointers, 1)

DECLARE_FEATURE_SET_PROMOTION(8BitStorage, 2)
DECLARE_FEATURE_SET_PROMOTION(ShaderFloat16Int8, 2)
DECLARE_FEATURE_SET_PROMOTION(DescriptorIndexing, 2)
DECLARE_FEATURE_SET_PROMOTION(BufferDeviceAddress, 2)
DECLARE_FEATURE_SET_PROMOTION(HostQueryReset, 2)
DECLARE_FEATURE_SET_PROMOTION(TimelineSemaphore, 2)
DECLARE_FEATURE_SET_PROMOTION(VulkanMemoryModel, 2)
DECLARE_FEATURE_SET_PROMOTION(ShaderSubgroupExtendedTypes, 2)
DECLARE_FEATURE_SET_PROMOTION(SeparateDepthStencilLayouts, 2)
DECLARE_FEATURE_SET_PROMOTION(UniformBufferStandardLayout, 2)
DECLARE_FEATURE_SET_PROMOTION(ShaderAtomicInt64, 2)
DECLARE_FEATURE_SET_PROMOTION(ScalarBlockLayout, 2)
DECLARE_FEATURE_SET_PROMOTION(ImagelessFramebuffer, 2)

DECLARE_FEATURE_SET_PROMOTION(Maintenance4, 3)
DECLARE_FEATURE_SET_PROMOTION(Synchronization2, 3)
DECLARE_FEATURE_SET_PROMOTION(DynamicRendering, 3)

template<>
struct FeatureSetTraits<VkPhysicalDeviceMeshShaderFeaturesEXT> final {
    static constexpr bool CORE = false;
    static constexpr bool PROMOTED = false;
};

#undef DECLARE_FEATURE_SET_PROMOTION
#undef DECLARE_FEATURE_SET_CORE

struct FeaturesNeedle final {
    PhysicalDeviceCapsStruct& caps;
    Version instance_version;
    void** pNext = nullptr;
    void* result = nullptr;

    template<typename FeatureSet>
    void thread(FeatureSet& field, VkStructureType const type) {
        using Traits = FeatureSetTraits<FeatureSet>;
        if constexpr(requires{ Traits::INTRODUCTION_VERSION; }) {
            if (Version{ 0, 1, Traits::INTRODUCTION_VERSION, 0 } > instance_version) {
                return;
            }
        }
        if constexpr(requires{ Traits::PROMOTION_VERSION; }) {
            if (Version{ 0, 1, Traits::PROMOTION_VERSION, 0 } <= instance_version) {
                return;
            }
        }
        if (pNext) {
            *pNext = &field;
        } else {
            result = &field;
        }
        field.sType = type;
        pNext = &field.pNext;
    }
};

template<typename FeatureSet>
auto& promotedIfAny(auto&& caps, FeatureSet PhysicalDeviceCapsStruct::*self) noexcept {
    using Traits = FeatureSetTraits<FeatureSet>;
    if constexpr (Traits::PROMOTED) {
        return caps.*Traits::CoreField;
    } else {
        return caps.*self;
    }
}

} // namespace

void* PhysicalDeviceCapsStruct::chained(Version instance_version) noexcept {
    FeaturesNeedle needle{ *this, instance_version };
    
    needle.thread(vulkan_11_features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    needle.thread(vulkan_12_features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    needle.thread(vulkan_13_features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    needle.thread(vulkan_14_features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);

    needle.thread(feat_16bit_storage,     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    needle.thread(feat_multiview,         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
    needle.thread(feat_variable_pointers, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES);

    needle.thread(feat_8bit_storage,                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES);
    needle.thread(feat_shader_float16_int8,            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    needle.thread(feat_descriptor_indexing,            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    needle.thread(feat_buffer_device_address,          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    needle.thread(feat_host_query_reset,               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
    needle.thread(feat_timeline_semaphore,             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    needle.thread(feat_vulkan_memory_model,            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES);
    needle.thread(feat_subgroup_extended_types,        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES);
    needle.thread(feat_separate_depth_stencil_layouts, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES);
    needle.thread(feat_uniform_buffer_standard_layout, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES);
    needle.thread(feat_shader_atomic_int64,            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES);
    needle.thread(feat_scalar_block_layout,            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES);
    needle.thread(feat_imageless_framebuffer,          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES);

    needle.thread(feat_maintenance4,      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES);
    needle.thread(feat_synchronization2,  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
    needle.thread(feat_dynamic_rendering, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);

    needle.thread(mesh_shader_features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT);
    
    // Sync extension and core structs
    for (VulkanFeature const feature : valuesOf<VulkanFeature>()) {
        if (hasFeature(feature)) {
            setFeature(feature);
        }
    }
    return needle.result;
}

PhysicalDeviceCapsStruct PhysicalDeviceCapsStruct::query(PhysicalDevice const& device, Version instance_version) {
    PhysicalDeviceCapsStruct caps{ };

    vkGetPhysicalDeviceProperties(device.handle(), &caps.properties);
    vkGetPhysicalDeviceMemoryProperties(device.handle(), &caps.memory_properties);

    if (vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceFeatures2 features2{ };
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = caps.chained(instance_version);
        vkGetPhysicalDeviceFeatures2(device.handle(), &features2);

        caps.features = features2.features;
    } else {
        vkGetPhysicalDeviceFeatures(device.handle(), &caps.features);
    }

    return caps;
}

bool PhysicalDeviceCapsStruct::hasFeature(VulkanFeature const feature) const noexcept {
    switch (feature) {
        using enum VulkanFeature;
#define RET_FEATURE(feature_set, field) \
    return false                        \
        || feature_set.field == VK_TRUE \
        || promotedIfAny(*this, &PhysicalDeviceCapsStruct::feature_set).field == VK_TRUE;

    case RobustBufferAccess:        return features.robustBufferAccess;
    case FullDrawIndexUInt32:       return features.fullDrawIndexUint32;
    case MultiDrawIndirect:         return features.multiDrawIndirect;
    case DrawIndirectFirstInstance: return features.drawIndirectFirstInstance;
    case SamplerAnisotropy:         return features.samplerAnisotropy;
    case FillModeNonSolid:          return features.fillModeNonSolid;
    case GeometryShader:            return features.geometryShader;
    case TesselationShader:         return features.tessellationShader;
    case ShaderFloat64:             return features.shaderFloat64;
    case ShaderInt64:               return features.shaderInt64;
    case ShaderInt16:               return features.shaderInt16;
    case PipelineStatisticsQuery:   return features.pipelineStatisticsQuery;

    case Multiview:                          RET_FEATURE(feat_multiview, multiview);
    case StorageBuffer16BitAccess:           RET_FEATURE(feat_16bit_storage, storageBuffer16BitAccess);
    case UniformAndStorageBuffer16BitAccess: RET_FEATURE(feat_16bit_storage, uniformAndStorageBuffer16BitAccess);
    case StoragePushConstant16:              RET_FEATURE(feat_16bit_storage, storagePushConstant16);
    case StorageInputOutput16:               RET_FEATURE(feat_16bit_storage, storageInputOutput16);
    case VariablePointers:                   RET_FEATURE(feat_variable_pointers, variablePointers);
    case VariablePointersStorageBuffer:      RET_FEATURE(feat_variable_pointers, variablePointersStorageBuffer);

    case TimelineSemaphore:                  RET_FEATURE(feat_timeline_semaphore, timelineSemaphore);
    case ShaderUniformBufferArrayNonUniformIndexing:
        RET_FEATURE(feat_descriptor_indexing, shaderUniformBufferArrayNonUniformIndexing);
    case ShaderSampledImageArrayNonUniformIndexing: 
        RET_FEATURE(feat_descriptor_indexing, shaderSampledImageArrayNonUniformIndexing);
    case ShaderStorageBufferArrayNonUniformIndexing:
        RET_FEATURE(feat_descriptor_indexing, shaderStorageBufferArrayNonUniformIndexing);
    case ShaderStorageImageArrayNonUniformIndexing: 
        RET_FEATURE(feat_descriptor_indexing, shaderStorageImageArrayNonUniformIndexing);
    case DescriptorBindingPartiallyBound:           
        RET_FEATURE(feat_descriptor_indexing, descriptorBindingPartiallyBound);
    case RuntimeDescriptorArray:                    
        RET_FEATURE(feat_descriptor_indexing, runtimeDescriptorArray);
    case DescriptorBindingVariableDescriptorCount:  
        RET_FEATURE(feat_descriptor_indexing, descriptorBindingVariableDescriptorCount);
    case BufferDeviceAddress:                RET_FEATURE(feat_buffer_device_address, bufferDeviceAddress);
    case HostQueryReset:                     RET_FEATURE(feat_host_query_reset, hostQueryReset);
    case ScalarBlockLayout:                  RET_FEATURE(feat_scalar_block_layout, scalarBlockLayout);
    case UniformBufferStandardLayout:        RET_FEATURE(feat_uniform_buffer_standard_layout, uniformBufferStandardLayout);
    case SeparateDepthStencilLayouts:        RET_FEATURE(feat_separate_depth_stencil_layouts, separateDepthStencilLayouts);

    case Synchronization2:                    RET_FEATURE(feat_synchronization2, synchronization2);
    case DynamicRendering:                    RET_FEATURE(feat_dynamic_rendering, dynamicRendering);
    case Maintanance4:                        RET_FEATURE(feat_maintenance4, maintenance4);
    case InlineUniformBlock:                  return vulkan_13_features.inlineUniformBlock;
    case PipelineCreationCacheControl:        return vulkan_13_features.pipelineCreationCacheControl;
    case ShaderDemoteToHelper:                return vulkan_13_features.shaderDemoteToHelperInvocation;
    case ShaderZeroInitializeWorkgroupMemory: return vulkan_13_features.shaderZeroInitializeWorkgroupMemory;
    case ComputeFullSubgroups:                return vulkan_13_features.computeFullSubgroups;
    case SubgroupSizeControl:                 return vulkan_13_features.subgroupSizeControl;
    case ShaderIntegerDotProduct:             return vulkan_13_features.shaderIntegerDotProduct;

    case PushDescriptor:       return vulkan_14_features.pushDescriptor;
    case Maintanance5:         return vulkan_14_features.maintenance5;
    case Maintanance6:         return vulkan_14_features.maintenance6;
    case ShaderFloatControls2: return vulkan_14_features.shaderFloatControls2;
    case ShaderSubgroupRotate: return vulkan_14_features.shaderSubgroupRotate;

    case TaskShader: RET_FEATURE(mesh_shader_features, taskShader);
    case MeshShader: RET_FEATURE(mesh_shader_features, meshShader);
#undef RET_FEATURE
    }
    UNREACHABLE("Unknown feature: {}", indexOf(feature));
}

void PhysicalDeviceCapsStruct::setFeature(VulkanFeature const feature, bool const value) noexcept {
    VkBool32 const v = value ? VK_TRUE : VK_FALSE;
    switch (feature) {
        using enum VulkanFeature;
#define SET_FEATURE(feature_set, field) \
    feature_set.field = v;              \
    promotedIfAny(*this, &PhysicalDeviceCapsStruct::feature_set).field = v; \
    break;

    case RobustBufferAccess:        features.robustBufferAccess = v; break;
    case FullDrawIndexUInt32:       features.fullDrawIndexUint32 = v; break;
    case MultiDrawIndirect:         features.multiDrawIndirect = v; break;
    case DrawIndirectFirstInstance: features.drawIndirectFirstInstance = v; break;
    case SamplerAnisotropy:         features.samplerAnisotropy = v; break;
    case FillModeNonSolid:          features.fillModeNonSolid = v; break;
    case GeometryShader:            features.geometryShader = v; break;
    case TesselationShader:         features.tessellationShader = v; break;
    case ShaderFloat64:             features.shaderFloat64 = v; break;
    case ShaderInt64:               features.shaderInt64 = v; break;
    case ShaderInt16:               features.shaderInt16 = v; break;
    case PipelineStatisticsQuery:   features.pipelineStatisticsQuery = v; break;

    case Multiview:                          SET_FEATURE(feat_multiview, multiview);
    case StorageBuffer16BitAccess:           SET_FEATURE(feat_16bit_storage, storageBuffer16BitAccess);
    case UniformAndStorageBuffer16BitAccess: SET_FEATURE(feat_16bit_storage, uniformAndStorageBuffer16BitAccess);
    case StoragePushConstant16:              SET_FEATURE(feat_16bit_storage, storagePushConstant16);
    case StorageInputOutput16:               SET_FEATURE(feat_16bit_storage, storageInputOutput16);
    case VariablePointers:                   SET_FEATURE(feat_variable_pointers, variablePointers);
    case VariablePointersStorageBuffer:      SET_FEATURE(feat_variable_pointers, variablePointersStorageBuffer);

    case TimelineSemaphore:                  SET_FEATURE(feat_timeline_semaphore, timelineSemaphore);
    case ShaderUniformBufferArrayNonUniformIndexing:
        SET_FEATURE(feat_descriptor_indexing, shaderUniformBufferArrayNonUniformIndexing);
    case ShaderSampledImageArrayNonUniformIndexing: 
        SET_FEATURE(feat_descriptor_indexing, shaderSampledImageArrayNonUniformIndexing);
    case ShaderStorageBufferArrayNonUniformIndexing:
        SET_FEATURE(feat_descriptor_indexing, shaderStorageBufferArrayNonUniformIndexing);
    case ShaderStorageImageArrayNonUniformIndexing: 
        SET_FEATURE(feat_descriptor_indexing, shaderStorageImageArrayNonUniformIndexing);
    case DescriptorBindingPartiallyBound:           
        SET_FEATURE(feat_descriptor_indexing, descriptorBindingPartiallyBound);
    case RuntimeDescriptorArray:                    
        SET_FEATURE(feat_descriptor_indexing, runtimeDescriptorArray);
    case DescriptorBindingVariableDescriptorCount:  
        SET_FEATURE(feat_descriptor_indexing, descriptorBindingVariableDescriptorCount);
    case BufferDeviceAddress:                SET_FEATURE(feat_buffer_device_address, bufferDeviceAddress);
    case HostQueryReset:                     SET_FEATURE(feat_host_query_reset, hostQueryReset);
    case ScalarBlockLayout:                  SET_FEATURE(feat_scalar_block_layout, scalarBlockLayout);
    case UniformBufferStandardLayout:        SET_FEATURE(feat_uniform_buffer_standard_layout, uniformBufferStandardLayout);
    case SeparateDepthStencilLayouts:        SET_FEATURE(feat_separate_depth_stencil_layouts, separateDepthStencilLayouts);

    case Synchronization2:                    SET_FEATURE(feat_synchronization2, synchronization2);
    case DynamicRendering:                    SET_FEATURE(feat_dynamic_rendering, dynamicRendering);
    case Maintanance4:                        SET_FEATURE(feat_maintenance4, maintenance4);
    case InlineUniformBlock:                  vulkan_13_features.inlineUniformBlock = v; break;
    case PipelineCreationCacheControl:        vulkan_13_features.pipelineCreationCacheControl = v; break;
    case ShaderDemoteToHelper:                vulkan_13_features.shaderDemoteToHelperInvocation = v; break;
    case ShaderZeroInitializeWorkgroupMemory: vulkan_13_features.shaderZeroInitializeWorkgroupMemory = v; break;
    case ComputeFullSubgroups:                vulkan_13_features.computeFullSubgroups = v; break;
    case SubgroupSizeControl:                 vulkan_13_features.subgroupSizeControl = v; break;
    case ShaderIntegerDotProduct:             vulkan_13_features.shaderIntegerDotProduct = v; break;

    case PushDescriptor:       vulkan_14_features.pushDescriptor = v; break;
    case Maintanance5:         vulkan_14_features.maintenance5 = v; break;
    case Maintanance6:         vulkan_14_features.maintenance6 = v; break;
    case ShaderFloatControls2: vulkan_14_features.shaderFloatControls2 = v; break;
    case ShaderSubgroupRotate: vulkan_14_features.shaderSubgroupRotate = v; break;

    case TaskShader: SET_FEATURE(mesh_shader_features, taskShader);
    case MeshShader: SET_FEATURE(mesh_shader_features, meshShader);
#undef SET_FEATURE
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
