#pragma once
#include <Core/Meta/UnRef.hpp>

namespace core {
    template<typename T>
    constexpr T&& forwardImpl(UnRef<T>& x) noexcept {
        return static_cast<T&&>(x);
    }
    template<typename T>
    constexpr T&& forwardImpl(UnRef<T>&& x) noexcept {
        return static_cast<T&&>(x);
    }
} // namespace core

#define FORWARD(...) ::core::forwardImpl<decltype(__VA_ARGS__)>(__VA_ARGS__)
