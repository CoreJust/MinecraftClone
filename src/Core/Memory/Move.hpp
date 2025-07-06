#pragma once
#include <Core/Meta/UnRef.hpp>

namespace core::memory {
    template<typename T>
    constexpr meta::UnRef<T>&& move(T&& x) noexcept {
        return static_cast<meta::UnRef<T>&&>(x);
    }
} // namespace core::memory
