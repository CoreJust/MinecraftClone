#include "Attribute.hpp"
#include <Core/IO/Logger.hpp>
#include <Core/Container/StringView.hpp>

namespace graphics::vulkan::pipeline {
    char const* Attribute::name() const {
        constexpr static char const* ATTRIBUTE_NAMES
            [static_cast<usize>(pipeline::Type::TypesCount)]
            [static_cast<usize>(pipeline::Bits::BitsCount)]
            [static_cast<usize>(pipeline::Format::FormatsCount)]
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
            || static_cast<usize>(format) >= static_cast<usize>(pipeline::Format::FormatsCount))
            return "Unknown";
        return ATTRIBUTE_NAMES[static_cast<usize>(type)][static_cast<usize>(bits)][static_cast<usize>(format)];
    }

    
    Attribute Attribute::of(char const* const attr) noexcept {
        static core::StringView formatStr[static_cast<usize>(Format::FormatsCount)] {
            "",
            "vec2",
            "vec3",
            "vec4",
            "mat2",
            "mat3",
            "mat4",
        };
        static core::StringView typeStr[static_cast<usize>(Type::TypesCount)] {
            "f",
            "i",
            "u",
            "scaled",
            "uscaled",
            "norm",
            "unorm",
            "srgb",
        };
        static core::StringView bitsStr[static_cast<usize>(Bits::BitsCount)] {
           "8",
           "16",
           "32",
           "64",
        };

        static auto parse = [](core::StringView& str, core::ArrayView<core::StringView const> values) -> u8 {
            auto it  = values.end() - 1;
            auto end = values.begin();
            while (it >= end) {
                if (str.startsWith(*it)) {
                    str = str.substr(it->size());
                    return static_cast<u8>(it - end);
                }
                --it;
            }
            core::fatal("Unrecognized attribute: {}", std::string_view { str.data(), str.size() });
            return static_cast<u8>(values.size());
        };

        core::StringView attrStr = attr;
        Format format = static_cast<Format>(parse(attrStr, formatStr));
        Type   type   = static_cast<Type>  (parse(attrStr, typeStr));
        Bits   bits   = static_cast<Bits>  (parse(attrStr, bitsStr));
        return {
            .type   = type,
            .bits   = bits,
            .format = format,
        };
    }
} // namespace graphics::vulkan::pipeline
