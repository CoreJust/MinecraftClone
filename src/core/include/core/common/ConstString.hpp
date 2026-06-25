#pragma once

#include <string_view>

namespace core {

template<size_t N>
struct ConstString final {
    char value[N]{};

    constexpr ConstString(const char (&str)[N]) {
        for (size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }
    
    constexpr operator std::string_view() const {
        return {value, N - 1};
    }
};

} // namespace core
