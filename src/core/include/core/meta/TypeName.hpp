#pragma once

#include <cstdint>
#include <string_view>

#if defined(__FUNCSIG__)
#   define PRETTY_FUNCTION __FUNCSIG__
#else
#   define PRETTY_FUNCTION __PRETTY_FUNCTION__
#endif

namespace core {
namespace name_extraction {

template<typename T>
consteval auto funcSelfNameDataT() {
    return std::string_view{ PRETTY_FUNCTION };
}

template<auto V>
consteval auto funcSelfNameDataV() {
    return std::string_view{ PRETTY_FUNCTION };
}

template<bool FullName>
consteval std::string_view strip(std::string_view const raw_name) {
#ifdef __FUNCSIG__
    size_t name_start = raw_name.find('<') + 1;
#else
    size_t name_start = raw_name.find_last_of('=') + 2;
#endif
    if constexpr (!FullName) {
        size_t const last_amp = raw_name.find_last_of('&');
        size_t const last_col = raw_name.find_last_of(':');
        if (last_amp != std::string_view::npos && last_amp >= name_start) {
            name_start = last_amp + 1;
        }
        if (last_col != std::string_view::npos && last_col >= name_start) {
            name_start = last_col + 1;
        }
    }
#ifdef __FUNCSIG__
    size_t const name_end = raw_name.find_last_of('>');
#else
    size_t const name_end = raw_name.find_last_of(']');
#endif
    return raw_name.substr(name_start, name_end - name_start);
}

} // namespace name_extraction

template<typename T>
consteval std::string_view typeName() noexcept {
    std::string_view result = name_extraction::funcSelfNameDataT<T>();
    return name_extraction::strip<false>(result);
}

template<auto V>
consteval std::string_view valueName() noexcept {
    std::string_view result = name_extraction::funcSelfNameDataV<V>();
    return name_extraction::strip<false>(result);
}

template<typename T>
consteval std::string_view fullTypeName() noexcept {
    std::string_view result = name_extraction::funcSelfNameDataT<T>();
    return name_extraction::strip<true>(result);
}

template<auto V>
consteval std::string_view fullValueName() noexcept {
    std::string_view result = name_extraction::funcSelfNameDataV<V>();
    return name_extraction::strip<true>(result);
}

} // namespace core
