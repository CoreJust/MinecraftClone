#pragma once

namespace core {
    struct NonCopyable {
        constexpr NonCopyable() noexcept = default;
        NonCopyable(NonCopyable&&) = default;
        NonCopyable& operator=(NonCopyable&&) = default;
    };
} // namespace core
