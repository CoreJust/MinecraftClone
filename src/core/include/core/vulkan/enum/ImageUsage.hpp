#pragma once

#include <core/common/EnumBits.hpp>
#include <core/meta/Enum.hpp>

namespace core {

enum class ImageUsage {
    TransferSrc,
    TransferDst,
    Sampled,
    Storage,
    ColorAttachment,
    DepthStencilAttachment,
    TransientAttachment,
    InputAttachment,
    FragmentShadingRateAttachment,
    FragmentDensityMap,
    VideoDecodeDst,
    VideoDecodeSrc,
    VideoDecodeDPB,
    VideoEncodeDst,
    VideoEncodeSrc,
    VideoEncodeDPB,
    Unused1,
    Unused2,
    InvocationMaskHuawei,
    AttachmentFeedbackLoop,
    SampleWeightQcom,
    SampleBlockMatchQcom,
    HostTransfer,
    TensorAliasingArm,
    Unused3,
    VideoEncodeQuantizationDeltaMap,
    VideoEncodeEmphasisMap,
    TileMemoryQcom,

    Count,
};

CORE_ENUM_FUNCTIONS(ImageUsage);

using ImageUsageBits = EnumBits<ImageUsage>;

} // namespace core
