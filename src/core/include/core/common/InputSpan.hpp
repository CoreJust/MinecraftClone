#pragma once

#include <initializer_list>
#include <span>

namespace core {

// Helper to be used in function parameters so they can
// accept normal spans and also inplace lists.
template<typename T>
struct InputSpan final {
    std::span<T const> data;

    constexpr InputSpan(std::span<T const> const data) noexcept
        : data(data)
    { }

    constexpr InputSpan(std::initializer_list<T> const data) noexcept
        : data(data.begin(), data.size())
    { }

    template<size_t N>
    constexpr InputSpan(T(&data)[N]) noexcept
        : data(data)
    { }

    [[nodiscard]]
    constexpr auto begin(this auto&& self) noexcept { return self.data.begin(); }
    [[nodiscard]]
    constexpr auto end(this auto&& self) noexcept { return self.data.end(); }
};

} // namespace core
