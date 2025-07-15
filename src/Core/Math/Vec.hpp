#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include <Core/Common/Assert.hpp>

namespace core {
    template<usize Size, typename T>
    struct Vec {
        static_assert(Size != 0);

        T data[Size] { };

        PURE constexpr bool operator==(Vec const&) const noexcept = default;

        PURE INLINE constexpr auto&& operator[](this auto&& self, auto idx) noexcept {
            ASSERT(idx >= 0 && static_cast<usize>(idx) < Size);
            return self.data[idx];
        }

#define DECL_VEC_EQ_OP(op)                                             \
        PURE constexpr Vec& operator op##=(Vec const& rhs) &noexcept { \
            T* dst = data, *src = rhs.data, *end = data + Size;        \
            while (dst < end) *(dst++) op##= *(src++);                 \
            return *this;                                              \
        }
        DECL_VEC_EQ_OP(+)
        DECL_VEC_EQ_OP(-)
#undef DECL_VEC_EQ_OP
#define DECL_SCALAR_EQ_OP(op)                                 \
        PURE constexpr Vec& operator op##=(T rhs) &noexcept { \
            T* dst = data, *end = data + Size;                \
            while (dst < end) *(dst++) op##= rhs;             \
            return *this;                                     \
        }
        DECL_SCALAR_EQ_OP(+)
        DECL_SCALAR_EQ_OP(-)
        DECL_SCALAR_EQ_OP(*)
        DECL_SCALAR_EQ_OP(/)
        DECL_SCALAR_EQ_OP(%)
#undef DECL_SCALAR_EQ_OP
#define DECL_VEC_OP(op, rhsTy) \
        PURE constexpr Vec operator op(rhsTy rhs) const noexcept { Vec tmp = *this; tmp op##= rhs; return tmp; }

        DECL_VEC_OP(+, Vec const&)
        DECL_VEC_OP(-, Vec const&)
        DECL_VEC_OP(+, T)
        DECL_VEC_OP(-, T)
        DECL_VEC_OP(*, T)
        DECL_VEC_OP(/, T)
        DECL_VEC_OP(%, T)
#undef DECL_VEC_OP

        template<typename U>
        PURE constexpr Vec<Size, U> to() const noexcept {
            Vec<Size, U> result { };
            T* dst = result.data, *src = data, *end = data + Size;
            while (src < end) *(dst++) = static_cast<U>(*(src++));
            return result;
        }

        template<unsigned int NewSize>
        PURE constexpr Vec<NewSize, T> resized(T fillWith = T { }) const noexcept {
            Vec<NewSize, T> result { };
            T* dst = result.data, *src = data, *resultEnd = result.data + NewSize;
            if constexpr (NewSize <= Size) {
                while (dst < resultEnd) *(dst++) = *(src++);
            } else {
                T* end = data + Size;
                while (src < end) *(dst++) = *(src++);
                while (dst < resultEnd) *(dst++) = fillWith;
            }
            return result;
        }

        template<unsigned int NewSize>
        PURE constexpr Vec<NewSize, T> head() const noexcept {
            static_assert(NewSize <= Size);
            return resized<NewSize>();
        }
    };

#define DECL_VEC_TYPES(n)                          \
    template<typename T> using Vec##n = Vec<n, T>; \
    using Vec##n##i8  = Vec<n, i8>;                \
    using Vec##n##i16 = Vec<n, i16>;               \
    using Vec##n##i32 = Vec<n, i32>;               \
    using Vec##n##i64 = Vec<n, i64>;               \
    using Vec##n##u8  = Vec<n, u8>;                \
    using Vec##n##u16 = Vec<n, u16>;               \
    using Vec##n##u32 = Vec<n, u32>;               \
    using Vec##n##u64 = Vec<n, u64>;               \
    using Vec##n##f   = Vec<n, float>;             \
    using Vec##n##d   = Vec<n, double>;
    DECL_VEC_TYPES(2)
    DECL_VEC_TYPES(3)
    DECL_VEC_TYPES(4)
#undef DECL_VEC_TYPES
} // namespace core
