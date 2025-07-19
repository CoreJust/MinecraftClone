#include "AttributeFormat.hpp"
#include <Core/Common/Assert.hpp>

namespace graphics::vulkan::internal {
    VkFormat attributeFormat(pipeline::Attribute attr) {
        VkFormat VULKAN_FORMATS
            [static_cast<usize>(pipeline::Type::TypesCount)]
            [static_cast<usize>(pipeline::Bits::BitsCount)]
            [static_cast<usize>(pipeline::Size::SizesCount)] 
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
            || static_cast<usize>(attr.size) >= static_cast<usize>(pipeline::Size::SizesCount))
            return VK_FORMAT_UNDEFINED;
        return VULKAN_FORMATS[static_cast<usize>(attr.type)][static_cast<usize>(attr.bits)][static_cast<usize>(attr.size)];
    }
} // namespace graphics::vulkan::internal
