#include <core/vulkan/enum/ImageLayout.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>

CORE_ENUM_FUNCTIONS_IMPL(vk::ImageLayout);

namespace core::vk {

uint32_t imageLayoutToVk(ImageLayout const layout) noexcept {
    static constexpr uint32_t VK_VALUES[] = {
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        1'000'117'000,
        1'000'117'001,
        1'000'241'000,
        1'000'241'001,
        1'000'241'002,
        1'000'241'003,
        1'000'314'000,
        1'000'314'001,
        1'000'232'000,
        1'000'001'002,
        1'000'024'000,
        1'000'024'001,
        1'000'024'002,
        1'000'111'000,
        1'000'218'000,
        1'000'164'003,
        1'000'299'000,
        1'000'299'001,
        1'000'299'002,
        1'000'339'000,
        1'000'460'000,
        1'000'553'000,
        1'000'620'000,
    };
    return VK_VALUES[indexOf(layout)];
}

ImageLayout imageLayoutFromVk(uint32_t const layout) noexcept {
    switch (layout) {
        case 0: return ImageLayout::Undefined;
        case 1: return ImageLayout::General;
        case 2: return ImageLayout::ColorAttachmentOptimal;
        case 3: return ImageLayout::DepthStencilAttachmentOptimal;
        case 4: return ImageLayout::DepthStencilReadOnlyOptimal;
        case 5: return ImageLayout::ShaderReadOnlyOptimal;
        case 6: return ImageLayout::TransferSrcOptimal;
        case 7: return ImageLayout::TransferDstOptimal;
        case 8: return ImageLayout::Preinitialized;
        case 1'000'117'000: return ImageLayout::DepthReadOnlyStencilAttachmentOptimal;
        case 1'000'117'001: return ImageLayout::DepthAttachmentStencilReadOnlyOptimal;
        case 1'000'241'000: return ImageLayout::DepthAttachmentOptimal;
        case 1'000'241'001: return ImageLayout::DepthReadOnlyOptimal;
        case 1'000'241'002: return ImageLayout::StencilAttachmentOptimal;
        case 1'000'241'003: return ImageLayout::StencilReadOnlyOptimal;
        case 1'000'314'000: return ImageLayout::ReadOnlyOptimal;
        case 1'000'314'001: return ImageLayout::AttachmentOptimal;
        case 1'000'232'000: return ImageLayout::RenderingLocalRead;
        case 1'000'001'002: return ImageLayout::PresentSrc;
        case 1'000'024'000: return ImageLayout::VideoDecodeDst;
        case 1'000'024'001: return ImageLayout::VideoDecodeSrc;
        case 1'000'024'002: return ImageLayout::VideoDecodeDpb;
        case 1'000'111'000: return ImageLayout::SharedPresent;
        case 1'000'218'000: return ImageLayout::FragmentDensityMapOptimal;
        case 1'000'164'003: return ImageLayout::FragmentShadingRateAttachmentOptimal;
        case 1'000'299'000: return ImageLayout::VideoEncodeDst;
        case 1'000'299'001: return ImageLayout::VideoEncodeSrc;
        case 1'000'299'002: return ImageLayout::VideoEncodeDpb;
        case 1'000'339'000: return ImageLayout::AttachmentFeedbackLoopOptimal;
        case 1'000'460'000: return ImageLayout::TensorAliasing;
        case 1'000'553'000: return ImageLayout::VideoEncodeQuantizationMap;
        case 1'000'620'000: return ImageLayout::ZeroInitialized;
    default: UNREACHABLE();
    }
}

} // namespace core::vk
