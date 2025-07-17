#pragma once
#include "Int.hpp"

namespace core {
    template<usize MaxValue = 0>
    consteval usize ENUM_SIZE() { 
        enum E { V = MaxValue };
        return sizeof(E);
    };
    
    template<usize MaxValue = 0>
    using DefaultUnderlyingEnumType = Int<ENUM_SIZE<MaxValue>()>;
} // namespace core
