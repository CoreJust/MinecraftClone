#pragma once
#include "Move.hpp"

namespace core::memory {
    auto exchange(auto& a, auto&& b) noexcept(noexcept(a = move(b))) {
        auto tmp = move(a);
        a = move(b);
        return tmp;
    }
} // namespace core::memory
