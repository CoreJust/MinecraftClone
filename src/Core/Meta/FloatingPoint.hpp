#pragma once
#include "IsSame.hpp"

namespace core {
    template<typename T>
    concept FloatingPoint = IsSame<T, float> || IsSame<T, double>;
} // namespace core
