#pragma once

#include <core/meta/Enum.hpp>

namespace core {

template<CountableEnum E> requires (countOf<E>() <= 32)
struct EnumBits final {
    uint32_t value = 0;

    template<typename... Args> [[nodiscard]]
    static constexpr EnumBits of(E const first, Args const... args) noexcept {
        if constexpr (sizeof...(Args) > 0) {
            return { (1u << indexOf(first)) | of(args...).value };
        } else {
            return { 1u << indexOf(first) };
        }
    }

    [[nodiscard]]
    constexpr bool operator[](E const bit) const noexcept {
        return value & (1u << indexOf(bit));
    }
};

} // namespace core
