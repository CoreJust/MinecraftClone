#include "VecOps.hpp"
#include <cmath>
#include <Core/Common/Int.hpp>
#include <Core/Common/Assert.hpp>

namespace core {
    template<typename T>
    T norm(core::ArrayView<T const> vec) noexcept {
        T sum = static_cast<T>(0);
        for (T const& t : vec)
            sum += t * t;
        return static_cast<T>(std::sqrt(sum));
    }

    template<typename T>
    T dot(core::ArrayView<T const> a, core::ArrayView<T const> b) noexcept {
        ASSERT(a.size() == b.size());

        T result = static_cast<T>(0);
        T const* bPtr = b.begin();
        for (T const& t : a)
            result += t * *(bPtr++);
        return result;
    }

    template<typename T>
    void cross(T const(&a)[3], T const(&b)[3], T(&c)[3]) noexcept {
        c[0] = a[1] * b[2] - a[2] * b[1];
        c[1] = a[2] * b[0] - a[0] * b[2];
        c[2] = a[0] * b[1] - a[1] * b[0];
    }

#define INSTANTIATE_VEC_OPS(type)                                                             \
    template type norm(core::ArrayView<type const> vec) noexcept;                             \
    template type dot(core::ArrayView<type const> a, core::ArrayView<type const> b) noexcept; \
    template void cross(type const(&a)[3], type const(&b)[3], type(&c)[3]) noexcept
    INSTANTIATE_VEC_OPS(i8);
    INSTANTIATE_VEC_OPS(i16);
    INSTANTIATE_VEC_OPS(i32);
    INSTANTIATE_VEC_OPS(i64);
    INSTANTIATE_VEC_OPS(u8);
    INSTANTIATE_VEC_OPS(u16);
    INSTANTIATE_VEC_OPS(u32);
    INSTANTIATE_VEC_OPS(u64);
    INSTANTIATE_VEC_OPS(float);
    INSTANTIATE_VEC_OPS(double);
#undef INSTANTIATE_VEC_OPS
} // namespace core
