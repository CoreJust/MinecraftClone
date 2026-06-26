#pragma once

namespace core {

struct NonMovable {
    constexpr NonMovable() noexcept = default;
    NonMovable(NonMovable&) = default;
    NonMovable& operator=(NonMovable&) = default;
};

} // namespace core
