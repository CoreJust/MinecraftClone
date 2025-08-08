#pragma once
#include <Core/Common/Int.hpp>

namespace graphics::vulkan::pipeline {
    enum class Type : u8 {
        Float,
        Int,
        Uint,
        // Signed integer that gets converted to float
        Scaled,
        // Unsigned integer that gets converted to float
        Uscaled,
        // Signed integer that gets converted to normalized float [-1; 1]
        Normalized,
        // Unsigned integer that gets converted to normalized float [0; 1]
        Unormalized,
        // The R, G, and B components are unsigned normalized values that represent values 
        // using sRGB nonlinear encoding, while the A component (if one exists) is a regular 
        // unsigned normalized value.
        SRGB,

        TypesCount,
    };

    enum class Bits : u8 {
        Bits8,
        Bits16,
        Bits32,
        Bits64,

        BitsCount,
    };

    enum class Format : u8 {
        Scalar,
        Vec2,
        Vec3,
        Vec4,
        Mat2,
        Mat3,
        Mat4,

        FormatsCount,
    };

    struct Attribute final {
        Type   type;
        Bits   bits;
        Format format;

        char const* name() const;
    
        static Attribute of(char const* const attr) noexcept;
    };
} // namespace graphics::vulkan::pipeline
