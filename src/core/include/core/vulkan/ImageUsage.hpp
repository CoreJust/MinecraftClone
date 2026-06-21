#pragma once

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

struct ImageUsageBits final {
    uint32_t value = 0;

    template<typename... Args> [[nodiscard]]
    static constexpr ImageUsageBits of(ImageUsage const first, Args const... args) noexcept {
        if constexpr (sizeof...(Args) > 0) {
            return { 1u << indexOf(first) | of(args...).value };
        } else {
            return { 1u << indexOf(first) };
        }
    }
};

} // namespace core
