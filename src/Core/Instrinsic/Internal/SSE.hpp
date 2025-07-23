#pragma once
#include <xmmintrin.h>
#include <Core/Macro/Attributes.hpp>
#include "SimdVec.hpp"

#define SIMD_ALIGNED ALIGNED(16)

namespace core {
    using f32 = float;
    using f64 = double;
#define DECL_SIMD_TYPE(type, size, base) using type##x##size = SimdVec<type, size, base>
#define DECL_SIMD_ITYPE(bits, size) DECL_SIMD_TYPE(i##bits, size, __m128i)
#define DECL_SIMD_UTYPE(bits, size) DECL_SIMD_TYPE(u##bits, size, __m128i)
    DECL_SIMD_TYPE(f32, 4, __m128);
    DECL_SIMD_TYPE(f64, 4, __m128);
    DECL_SIMD_ITYPE(8, 16);
    DECL_SIMD_ITYPE(16, 8);
    DECL_SIMD_ITYPE(32, 4);
    DECL_SIMD_ITYPE(64, 2);
    DECL_SIMD_UTYPE(8, 16);
    DECL_SIMD_UTYPE(16, 8);
    DECL_SIMD_UTYPE(32, 4);
    DECL_SIMD_UTYPE(64, 2);
#undef DECL_SIMD_ITYPE
#undef DECL_SIMD_TYPE
#undef DECL_SIMD_TYPE


} // namespace core
