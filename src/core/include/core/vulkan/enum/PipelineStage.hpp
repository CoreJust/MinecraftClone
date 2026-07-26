#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

#include <cstdint>

namespace core::vk {

enum class PipelineStage {
    TopOfPipe,
    DrawIndirect,
    VertexInput,
    VertexShader,
    TessellationControlShader,
    TessellationEvaluationShader,
    GeometryShader,
    FragmentShader,
    EarlyFragmentTests,
    LateFragmentTests,
    ColorAttachmentOutput,
    ComputeShader,
    Transfer,
    BottomOfPipe,
    Host,
    AllGraphics,
    AllCommands,
    CommandPreprocess,
    ConditionalRendering,
    TaskShader,
    MeshShader,
    RayTracingShader,
    ShadingRate,
    FragmentDensity,
    TransformFeedback,
    AccelerationStructureBuild,
    VideoDecode,
    VideoEncode,
    AccelerationStructureCopy,
    OpticalFlow,
    MicromapBuild,
    Unused0,
    Copy,
    Resolve,
    Blit,
    Clear,
    IndexInput,
    VertexAttribute,
    PreRasterization,
    SubpassShader,
    InvocationMask,
    ClusterCulling,
    DataGraph,
    Unused1,
    ConvertCooperativeVectorMatrix,
    MemoryDecompression,
    CopyIndirect,

    Count,
};

using PipelineStages = EnumBits<PipelineStage, uint64_t>;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::PipelineStage);
