#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>
#include <Core/Common/Assert.hpp>
#include <Core/Container/ArrayView.hpp>
#include "VecOps.hpp"

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

        PURE constexpr operator core::ArrayView<T>      ()       noexcept { return { data }; }
        PURE constexpr operator core::ArrayView<T const>() const noexcept { return { data }; }

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
        PURE Vec<NewSize, T>& slice() &noexcept {
            static_assert(NewSize + From <= Size);
            return *reinterpret_cast<Vec<NewSize, T>*>(data + From);
        }

        template<usize NewSize, usize From = 0>
        PURE Vec<NewSize, T> const& slice() const& noexcept {
            static_assert(NewSize + From <= Size);
            return *reinterpret_cast<Vec<NewSize, T> const*>(data + From);
        }

        template<usize... Idx>
        PURE Vec swizzled() const noexcept {
            constexpr static usize indices[] = { Idx... };
            Vec result = *this;
            for (usize i = 0; i < sizeof...(Idx); ++i) 
                result.data[i] = data[indices[i]];
            return result;
        }

        PURE T norm() const noexcept {
            return core::norm(core::ArrayView<T const>(data));
        }

        PURE Vec normalized() const noexcept {
            return *this / norm();
        }

        PURE T dot(Vec const& rhs) const noexcept {
            return core::dot(core::ArrayView<T const>(data), core::ArrayView<T const>(rhs.data));
        }

        PURE Vec cross(Vec const& rhs) const noexcept {
            Vec result;
            core::cross(data, rhs.data, result.data);
            return result;
        }

#define DECL_COMPONENT(name, idx)                                                                 \
        PURE constexpr T      & name()       &noexcept requires(Size > idx) { return data[idx]; } \
        PURE constexpr T const& name() const &noexcept requires(Size > idx) { return data[idx]; }
#define DECL_ROTATION_COMPONENT(name, idx, dims)                                                    \
        PURE constexpr T      & name()       &noexcept requires(Size == dims) { return data[idx]; } \
        PURE constexpr T const& name() const &noexcept requires(Size == dims) { return data[idx]; }

        DECL_COMPONENT(x, 0)
        DECL_COMPONENT(y, 1)
        DECL_COMPONENT(z, 2)
        DECL_COMPONENT(w, 3) 
        DECL_COMPONENT(r, 0)
        DECL_COMPONENT(g, 1)
        DECL_COMPONENT(b, 2)
        DECL_COMPONENT(a, 3) 
        DECL_ROTATION_COMPONENT(roll,  0, 3)
        DECL_ROTATION_COMPONENT(pitch, 1, 3)
        DECL_ROTATION_COMPONENT(yaw,   2, 3)
#undef DECL_ROTATION_COMPONENT
#undef DECL_COMPONENT

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
