#pragma once

#include <core/meta/Enum.hpp>

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
    DepthReadOnlyStencilAttachmentOptimal,
    DepthAttachmentStencilReadOnlyOptimal,
    DepthAttachmentOptimal,
    DepthReadOnlyOptimal,
    StencilAttachmentOptimal,
    StencilReadOnlyOptimal,
    ReadOnlyOptimal,
    AttachmentOptimal,
    RenderingLocalRead,
    PresentSrc,
    VideoDecodeDst,
    VideoDecodeSrc,
    VideoDecodeDpb,
    SharedPresent,
    FragmentDensityMapOptimal,
    FragmentShadingRateAttachmentOptimal,
    VideoEncodeDst,
    VideoEncodeSrc,
    VideoEncodeDpb,
    AttachmentFeedbackLoopOptimal,
    TensorAliasing,
    VideoEncodeQuantizationMap,
    ZeroInitialized,
    
    Count,
};

[[nodiscard]]
uint32_t imageLayoutToVk(ImageLayout const layout) noexcept;
[[nodiscard]]
ImageLayout imageLayoutFromVk(uint32_t const layout) noexcept;

} // namespace core::vk

CORE_ENUM_FUNCTIONS(::core::vk::ImageLayout);
