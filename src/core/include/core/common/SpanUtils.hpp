#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace core {

auto unitSpan(auto& single_element) {
    return std::span{ &single_element, 1 };
}

auto asByteSpan(auto&& collection)
    requires requires { collection.data(); collection.size(); } {
    using Element = std::remove_reference_t<decltype(*collection.data())>;
    if constexpr (std::is_const_v<Element>) {
        return std::span{ reinterpret_cast<uint8_t const*>(collection.data()), collection.size() * sizeof(Element) };
    } else {
        return std::span{ reinterpret_cast<uint8_t*>(collection.data()), collection.size() * sizeof(Element) };
    }
}

std::string_view asStringView(auto const collection)
    requires requires { collection.data(); collection.size(); } {
    using Element = decltype((*collection.data()));
    return std::string_view{ reinterpret_cast<char const*>(collection.data()), collection.size() * sizeof(Element) };
}

template<typename First, typename... Args>
struct SpanOver final {
    First data[1 + sizeof...(Args)];

    constexpr SpanOver(First first, Args... args) noexcept
        : data{ std::move(first), std::move(args)... }
    { }

    constexpr auto asSpan() & noexcept { return std::span(data, 1 + sizeof...(Args)); };
};

template<typename T>
struct SpanOver<std::span<T>> final {
    std::span<T> data;;
    constexpr auto asSpan() & noexcept { return data; };
};

template<typename T>
struct SpanOver<std::vector<T>> final {
    std::vector<T> const& data;;
    constexpr std::span<T> asSpan() & noexcept { return data; };
};

} // namespace core
