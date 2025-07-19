#pragma once
#include <Core/Common/Int.hpp>

namespace core {
    template<typename T, usize N>
    constexpr bool contains(T(&arr)[N], T const& value) noexcept {
        for (auto const& elem : arr) {
            if (elem == value)
                return true;
        }
        return false;
    }
} // namespace core
