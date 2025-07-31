#pragma once
#include "Move.hpp"

namespace core {
    constexpr auto exchange(auto& a, auto&& b) noexcept(noexcept(a = core::move(b))) {
        auto tmp = core::move(a);
        a = core::move(b);
        return tmp;
    }
} // namespace core
