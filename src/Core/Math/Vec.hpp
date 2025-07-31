#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include <Core/Common/Assert.hpp>

namespace core {
    template<usize Size_, typename T>
    struct Vec {
        constexpr static inline usize Size = Size_;
        static_assert(Size != 0);

        T data[Size] { };

        PURE constexpr static Vec constant(T value) noexcept {
            Vec result { };
            for (usize i = 0; i < Size; ++i)
                result.data[i] = value;
            return result;
        }

        PURE constexpr static Vec zero() noexcept { return constant(static_cast<T>(0)); }

        PURE constexpr static Vec constantAt(usize index, T value) noexcept {
            Vec result = zero();
            result.data[index] = value;
            return result;
        }
        
        PURE constexpr static Vec unit(usize index) noexcept { return constantAt(index, static_cast<T>(1)); }

        PURE constexpr bool operator==(Vec const&) const noexcept = default;

        PURE INLINE constexpr T& operator[](usize idx) noexcept {
            ASSERT(idx >= 0 && static_cast<usize>(idx) < Size);
            return data[idx];
        }

        PURE INLINE constexpr T const& operator[](usize idx) const noexcept {
            ASSERT(idx >= 0 && static_cast<usize>(idx) < Size);
            return data[idx];
        }

        PURE INLINE constexpr T      * elemPtr(usize idx)       noexcept { return &data[idx]; }
        PURE INLINE constexpr T const* elemPtr(usize idx) const noexcept { return &data[idx]; }

        constexpr Vec operator-() const noexcept {
            Vec result = *this;
            for (T& elem : result)
                elem = -elem;
            return result;
        }

#define DECL_VEC_EQ_OP(op)                                             \
        constexpr Vec& operator op##=(Vec const& rhs) &noexcept {      \
            T* dst = data, *end = data + Size;                         \
            T const* src = rhs.data;                                   \
            while (dst < end) *(dst++) op##= *(src++);                 \
            return *this;                                              \
        }
        DECL_VEC_EQ_OP(+)
        DECL_VEC_EQ_OP(-)
#undef DECL_VEC_EQ_OP
#define DECL_SCALAR_EQ_OP(op)                                 \
        constexpr Vec& operator op##=(T rhs) &noexcept {      \
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
            T* dst = result.data, *resultEnd = result.data + NewSize;
            T const* src = data;
            if constexpr (NewSize <= Size) {
                while (dst < resultEnd) *(dst++) = *(src++);
            } else {
                T const* end = data + Size;
                while (src < end) *(dst++) = *(src++);
                while (dst < resultEnd) *(dst++) = fillWith;
            }
            return result;
        }

        template<usize NewSize, usize From = 0>
        PURE constexpr Vec<NewSize, T> slice() const noexcept {
            static_assert(NewSize + From <= Size);
            Vec<NewSize, T> result { };
            T* dst = result.data, *resultEnd = result.data + NewSize + From;
            T const* src = data + From;
            while (dst < resultEnd) *(dst++) = *(src++);
            return result;
        }

        PURE constexpr T      * begin ()       noexcept { return data; }
        PURE constexpr T const* begin () const noexcept { return data; }
        PURE constexpr T const* cbegin() const noexcept { return data; }
        PURE constexpr T      * end   ()       noexcept { return data + Size; }
        PURE constexpr T const* end   () const noexcept { return data + Size; }
        PURE constexpr T const* cend  () const noexcept { return data + Size; }
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
