// core/vulkan/enum/Access.hpp
#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core::vk {

enum class AccessFlag {
    IndirectCommandRead,
    IndexRead,
    VertexAttributeRead,
    UniformRead,
    InputAttachmentRead,
    ShaderRead,
    ShaderWrite,
    ColorAttachmentRead,
    ColorAttachmentWrite,
    DepthStencilAttachmentRead,
    DepthStencilAttachmentWrite,
    TransferRead,
    TransferWrite,
    HostRead,
    HostWrite,
    MemoryRead,
    MemoryWrite,
    CommandPreprocessRead,
    CommandPreprocessWrite,
    ColorAttachmentReadIncoherent,
    ConditionalRenderingRead,
    AccelerationStructureRead,
    AccelerationStructureWrite,
    ShadingRateImageRead,
    FragmentDensityMapRead,
    TransformFeedbackWrite,
    TransformFeedbackCounterRead,
    TransformFeedbackCounterWrite,
    Unused0,
    Unused1,
    Unused2,
    Unused3,
    ShaderSamplerRead,
    ShaderStorageRead,
    ShaderStorageWrite,
    VideoDecodeRead,
    VideoDecodeWrite,
    VideoEncodeRead,
    VideoEncodeWrite,
    InvocationMaskRead,
    ShaderBindingTableRead,
    DescriptorBufferRead,
    OpticalFlowRead,
    OpticalFlowWrite,
    MicromapRead,
    MicromapWrite,
    Unused4,
    DataGraphRead,
    DataGraphWrite,
    Unused5,
    Unused6,
    ShaderTileAttachmentRead,
    ShaderTileAttachmentWrite,
    Unused7,
    Unused8,
    MemoryDecompressionRead,
    MemoryDecompressionWrite,
    SamplerHeapRead,
    ResourceHeapRead,

    Count,
};

using AccessFlags = EnumBits<AccessFlag, uint64_t>;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(vk::AccessFlag);
