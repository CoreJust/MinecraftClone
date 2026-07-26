#pragma once

#include <core/vulkan/enum/VulkanEnum.hpp>

#include <cstdint>

namespace core::vk {

enum class ImageLayout {
    Undefined,
    General,
    ColorAttachmentOptimal,
    DepthStencilAttachmentOptimal,
    DepthStencilReadOnlyOptimal,
    ShaderReadOnlyOptimal,
    TransferSrcOptimal,
    TransferDstOptimal,
    Preinitialized,
    PresentSrc,
    VideoDecodeDst,
    VideoDecodeSrc,
    VideoDecodeDpb,
    SharedPresent,
    DepthReadOnlyStencilAttachmentOptimal,
    DepthAttachmentStencilReadOnlyOptimal,
    FragmentShadingRateAttachmentOptimal,
    FragmentDensityMapOptimal,
    RenderingLocalRead,
    DepthAttachmentOptimal,
    DepthReadOnlyOptimal,
    StencilAttachmentOptimal,
    StencilReadOnlyOptimal,
    VideoEncodeDst,
    VideoEncodeSrc,
    VideoEncodeDpb,
    ReadOnlyOptimal,
    AttachmentOptimal,
    AttachmentFeedbackLoopOptimal,
    TensorAliasing,
    VideoEncodeQuantizationMap,
    ZeroInitialized,
    
    Count,
};

} // namespace core::vk

CORE_VK_REGISTER_ENUM(ImageLayout,
    { PresentSrc,                            1'000'001'002 },
    { VideoDecodeDst,                        1'000'024'000 },
    { SharedPresent,                         1'000'111'000 },
    { DepthReadOnlyStencilAttachmentOptimal, 1'000'117'000 },
    { FragmentShadingRateAttachmentOptimal,  1'000'164'003 },
    { FragmentDensityMapOptimal,             1'000'218'003 },
    { RenderingLocalRead,                    1'000'232'000 },
    { DepthAttachmentOptimal,                1'000'241'000 },
    { VideoEncodeDst,                        1'000'299'000 },
    { ReadOnlyOptimal,                       1'000'314'000 },
    { AttachmentFeedbackLoopOptimal,         1'000'339'000 },
    { TensorAliasing,                        1'000'460'000 },
    { VideoEncodeQuantizationMap,            1'000'553'000 },
    { ZeroInitialized,                       1'000'620'000 }
);
