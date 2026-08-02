#pragma once

namespace core {

struct NonMovable {
    constexpr NonMovable() noexcept = default;
    NonMovable(NonMovable const&) = default;
    NonMovable& operator=(NonMovable const&) = default;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

} // namespace core
