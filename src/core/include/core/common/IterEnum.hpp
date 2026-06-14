#pragma once

#include <cstdint>
#include <ranges>

namespace core {

template <typename E>
    requires requires { E::Count; }
auto iterEnum() {
    return std::views::iota(0u, static_cast<uint32_t>(E::Count))
         | std::views::transform([](uint32_t const i) { return static_cast<E>(i); });
}

} // namespace core
