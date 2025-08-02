#pragma once
#include "UnRef.hpp"

namespace core {
    template<typename T>
    struct DecayImpl {
        using Type = T;
    };
    template<typename T, unsigned long long N>
    struct DecayImpl<T[N]> {
        using Type = T*;
    };
    template<typename T>
    struct DecayImpl<T[]> {
        using Type = T*;
    };
    template<typename R, typename... Args>
    struct DecayImpl<R(Args...)> {
        using Type = R(*)(Args...);
    };

    template<typename T>
    using Decay = DecayImpl<core::UnRef<T>>::Type;
} // namespace core
