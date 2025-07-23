#pragma once
#include <Core/Common/Int.hpp>

namespace core {
    template<typename T, usize S, typename Base>
    struct SimdVec final {
        using Type = T;
        constexpr inline static usize Size = S;

        static_assert(sizeof(Base) == Size * sizeof(T))

        Base value;
    };
} // namespace core
