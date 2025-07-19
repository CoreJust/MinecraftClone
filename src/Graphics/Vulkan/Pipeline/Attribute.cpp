#include "Attribute.hpp"

namespace graphics::vulkan::pipeline {
    char const* Attribute::name() const {
        constexpr static char const* ATTRIBUTE_NAMES
            [static_cast<usize>(pipeline::Type::TypesCount)]
            [static_cast<usize>(pipeline::Bits::BitsCount)]
            [static_cast<usize>(pipeline::Size::SizesCount)]
        = {
#define DECL_ATTR_NAME_IMPL(type, bits, size) #size "<" #type #bits ">"
#define DECL_ATTR_NAME_TYPE_BITS(type, bits) { \
    DECL_ATTR_NAME_IMPL(type, bits, Scalar),   \
    DECL_ATTR_NAME_IMPL(type, bits, Vec2),     \
    DECL_ATTR_NAME_IMPL(type, bits, Vec3),     \
    DECL_ATTR_NAME_IMPL(type, bits, Vec4),     \
    DECL_ATTR_NAME_IMPL(type, bits, Mat2),     \
    DECL_ATTR_NAME_IMPL(type, bits, Mat3),     \
    DECL_ATTR_NAME_IMPL(type, bits, Mat4) }
#define DECL_ATTR_NAME_TYPE(type) {     \
    DECL_ATTR_NAME_TYPE_BITS(type, 8),  \
    DECL_ATTR_NAME_TYPE_BITS(type, 16), \
    DECL_ATTR_NAME_TYPE_BITS(type, 32), \
    DECL_ATTR_NAME_TYPE_BITS(type, 64) }

        DECL_ATTR_NAME_TYPE(Float),
        DECL_ATTR_NAME_TYPE(Int),
        DECL_ATTR_NAME_TYPE(Uint),
        DECL_ATTR_NAME_TYPE(Scaled),
        DECL_ATTR_NAME_TYPE(Uscaled),
        DECL_ATTR_NAME_TYPE(Normalized),
        DECL_ATTR_NAME_TYPE(Unormalized),
        DECL_ATTR_NAME_TYPE(SRGB),

#undef DECL_ATTR_NAME_TYPE_SIZE
#undef DECL_ATTR_NAME_TYPE
#undef DECL_ATTR_NAME_IMPL
        };

        if (static_cast<usize>(type) >= static_cast<usize>(pipeline::Type::TypesCount)
            || static_cast<usize>(bits) >= static_cast<usize>(pipeline::Bits::BitsCount)
            || static_cast<usize>(size) >= static_cast<usize>(pipeline::Size::SizesCount))
            return "Unknown";
        return ATTRIBUTE_NAMES[static_cast<usize>(type)][static_cast<usize>(bits)][static_cast<usize>(size)];
    }
} // namespace graphics::vulkan::pipeline
