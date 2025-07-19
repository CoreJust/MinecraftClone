#pragma once
#include <Core/Common/Int.hpp>

namespace core {
    template<typename T>
    constexpr bool IsArray = false;
    template<typename T, usize N>
    constexpr bool IsArray<T[N]> = true;
    template<typename T, usize N>
    constexpr bool IsArray<T[]> = true;
    
    template<typename T>
    concept ArrayConcept = IsArray<T>;
} // namespace core
