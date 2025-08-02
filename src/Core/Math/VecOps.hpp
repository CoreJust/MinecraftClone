#pragma once
#include <Core/Container/ArrayView.hpp>

namespace core {
    template<typename T>
    T norm(core::ArrayView<T const> vec) noexcept;

    template<typename T>
    T dot(core::ArrayView<T const> a, core::ArrayView<T const> b) noexcept;

    template<typename T>
    void cross(T const(&a)[3], T const(&b)[3], T(&c)[3]) noexcept;
} // namespace core
