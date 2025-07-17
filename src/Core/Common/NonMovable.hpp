#pragma once

namespace core {
    struct NonMovable {
        constexpr NonMovable() noexcept = default;
        NonMovable(NonMovable&&) = delete;
        NonMovable& operator=(NonMovable&&) = delete;
    };
} // namespace core
