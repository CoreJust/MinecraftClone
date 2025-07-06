#pragma once
#include <Core/Meta/UnRef.hpp>

namespace core::memory {
    template<typename T>
    constexpr T&& forwardImpl(meta::UnRef<T>& x) noexcept {
        return static_cast<T&&>(x);
    }
    template<typename T>
    constexpr T&& forwardImpl(meta::UnRef<T>&& x) noexcept {
        return static_cast<T&&>(x);
    }
} // namespace core::memory

#define FORWARD(...) ::core::memory::forwardImpl<decltype(__VA_ARGS__)>(__VA_ARGS__)
