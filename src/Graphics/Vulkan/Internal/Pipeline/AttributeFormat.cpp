#include "AttributeFormat.hpp"
#include <Core/Common/Assert.hpp>

namespace graphics::vulkan::internal {
    VkFormat attributeFormat(pipeline::Attribute attr) {
        constexpr static VkFormat VULKAN_FORMATS
            [static_cast<usize>(pipeline::Type::TypesCount)]
            [static_cast<usize>(pipeline::Bits::BitsCount)]
            [static_cast<usize>(pipeline::Format::FormatsCount)] 
        = {
#define DECL_VK_FORMAT_IMPL(componentFormat, numericFormat) VK_FORMAT_##componentFormat##_##numericFormat
#define DECL_VK_FORMAT1_IMPL(bits, format) DECL_VK_FORMAT_IMPL(R##bits,                            format)
#define DECL_VK_FORMAT2_IMPL(bits, format) DECL_VK_FORMAT_IMPL(R##bits##G##bits,                   format)
#define DECL_VK_FORMAT3_IMPL(bits, format) DECL_VK_FORMAT_IMPL(R##bits##G##bits##B##bits,          format)
#define DECL_VK_FORMAT4_IMPL(bits, format) DECL_VK_FORMAT_IMPL(R##bits##G##bits##B##bits##A##bits, format)
#define DECL_VK_FORMAT(format, bits, size) DECL_VK_FORMAT##size##_IMPL(bits, format)
#define DECL_VK_FORMATS_FORMAT_BITS(format, bits) {   \
    DECL_VK_FORMAT(format, bits, 1),              \
    DECL_VK_FORMAT(format, bits, 2),              \
    DECL_VK_FORMAT(format, bits, 3),              \
    DECL_VK_FORMAT(format, bits, 4),              \
    DECL_VK_FORMAT(format, bits, 4),              \
    VK_FORMAT_UNDEFINED, /* couldn't pass mat3 */ \
    DECL_VK_FORMAT(format, bits, 4) }
#define DECL_VK_FORMATS_UNDEFINED() { \
    VK_FORMAT_UNDEFINED,          \
    VK_FORMAT_UNDEFINED,          \
    VK_FORMAT_UNDEFINED,          \
    VK_FORMAT_UNDEFINED,          \
    VK_FORMAT_UNDEFINED,          \
    VK_FORMAT_UNDEFINED,          \
    VK_FORMAT_UNDEFINED }
#define DECL_VK_FORMATS_FORMAT(format) {         \
    DECL_VK_FORMATS_FORMAT_BITS(format, 8),  \
    DECL_VK_FORMATS_FORMAT_BITS(format, 16), \
    DECL_VK_FORMATS_FORMAT_BITS(format, 32), \
    DECL_VK_FORMATS_FORMAT_BITS(format, 64) }
#define DECL_VK_FORMATS_8_16_FORMAT(format) {    \
    DECL_VK_FORMATS_FORMAT_BITS(format, 8),  \
    DECL_VK_FORMATS_FORMAT_BITS(format, 16), \
    DECL_VK_FORMATS_UNDEFINED(),             \
    DECL_VK_FORMATS_UNDEFINED() }
#define DECL_VK_FLOAT_FORMATS_FORMAT() {         \
    DECL_VK_FORMATS_UNDEFINED(),             \
    DECL_VK_FORMATS_FORMAT_BITS(SFLOAT, 16), \
    DECL_VK_FORMATS_FORMAT_BITS(SFLOAT, 32), \
    DECL_VK_FORMATS_FORMAT_BITS(SFLOAT, 64) }
#define DECL_VK_SRGB_FORMATS_FORMAT() {       \
    DECL_VK_FORMATS_FORMAT_BITS(SRGB, 8), \
    DECL_VK_FORMATS_UNDEFINED(),          \
    DECL_VK_FORMATS_UNDEFINED(),          \
    DECL_VK_FORMATS_UNDEFINED() }
            
            DECL_VK_FLOAT_FORMATS_FORMAT(),
            DECL_VK_FORMATS_FORMAT(SINT),
            DECL_VK_FORMATS_FORMAT(UINT),
            DECL_VK_FORMATS_8_16_FORMAT(SSCALED),
            DECL_VK_FORMATS_8_16_FORMAT(USCALED),
            DECL_VK_FORMATS_8_16_FORMAT(SNORM),
            DECL_VK_FORMATS_8_16_FORMAT(UNORM),
            DECL_VK_SRGB_FORMATS_FORMAT(),

#undef DECL_VK_FLOAT_FORMATS_FORMAT
#undef DECL_VK_FORMATS_8_16_FORMAT
#undef DECL_VK_FORMATS_FORMAT
#undef DECL_VK_FORMATS_UNDEFINED
#undef DECL_VK_FORMATS_FORMAT_BITS
#undef DECL_VK_FORMAT
#undef DECL_VK_FORMAT4_IMPL
#undef DECL_VK_FORMAT3_IMPL
#undef DECL_VK_FORMAT2_IMPL
#undef DECL_VK_FORMAT1_IMPL
#undef DECL_VK_FORMAT_IMPL
        };
    
        if (static_cast<usize>(attr.type) >= static_cast<usize>(pipeline::Type::TypesCount)
            || static_cast<usize>(attr.bits) >= static_cast<usize>(pipeline::Bits::BitsCount)
            || static_cast<usize>(attr.format) >= static_cast<usize>(pipeline::Format::FormatsCount))
            return VK_FORMAT_UNDEFINED;
        return VULKAN_FORMATS[static_cast<usize>(attr.type)][static_cast<usize>(attr.bits)][static_cast<usize>(attr.format)];
    }

    u32 attributeSize(pipeline::Attribute attr) {
        constexpr static u32 SIZES[] = { 1, 2, 3, 4, 4, 9, 16, };
        u32 const bitsSize = (1 << static_cast<u32>(attr.bits));
        return bitsSize * SIZES[static_cast<usize>(attr.format)];
    }

    u32 attributeLocationSize(pipeline::Attribute attr) {
        switch (attr.format) {
            case pipeline::Format::Mat4: return 4;
        default: return 1;
        }
    }

    VulkanVertexLayoutDescription makeVertexLayoutDescription(core::ArrayView<pipeline::Attribute const> attrs) {
        u32 location = 0, offset = 0;
        core::DynArray<VkVertexInputAttributeDescription> attrDescriptions(attrs.size());
        for (usize i = 0; i < attrs.size(); ++i) {
            pipeline::Attribute const& attr = attrs[i];
            VkVertexInputAttributeDescription& desc = attrDescriptions[i];
            desc.binding = 0;
            desc.location = location;
            desc.offset = offset;
            desc.format = attributeFormat(attr);
            location += attributeLocationSize(attr);
            offset += attributeSize(attr);
        }
        
        VkVertexInputBindingDescription bindingDescription {
            .binding = 0,
            .stride = offset,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };

        return {
            .attrDescriptions = core::move(attrDescriptions),
            .bindingDescription = core::move(bindingDescription),
        };
    }

    VkPushConstantRange makePushConstantsDescription(ShaderStageBit stage, core::ArrayView<pipeline::Attribute const> attrs) {
        u32 offset = 0;
        for (auto const& attr : attrs)
            offset += attributeSize(attr);

        return {
            .stageFlags = static_cast<VkShaderStageFlags>(stage),
            .offset = 0,
            .size = offset,
        };
    }
} // namespace graphics::vulkan::internal
