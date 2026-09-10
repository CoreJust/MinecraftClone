#pragma once

#include <ranges>
#include <span>
#include <string_view>

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

template<typename U>
auto asSpan(auto&& collection)
    requires requires { collection.data(); collection.size(); } {
    using Element = std::remove_reference_t<decltype(*collection.data())>;
    if constexpr (std::is_const_v<Element>) {
        return std::span{ reinterpret_cast<U const*>(collection.data()), collection.size() * sizeof(Element) / sizeof(U) };
    } else {
        return std::span{ reinterpret_cast<U*>(collection.data()), collection.size() * sizeof(Element) / sizeof(U) };
    }
}

template<typename Collection>
[[nodiscard]]
std::string_view asStringView(Collection&& collection)
    requires requires {
        collection.data();
        collection.size();
        requires std::ranges::borrowed_range<Collection>;
    }
{
    using Element = decltype((*collection.data()));
    return std::string_view{ reinterpret_cast<char const*>(collection.data()), collection.size() * sizeof(Element) };
}

} // namespace core
