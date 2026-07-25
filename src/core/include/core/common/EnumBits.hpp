#pragma once

#include <core/meta/Enum.hpp>

namespace core {

template<CountableEnum E, typename UnderlyingTy = uint32_t>
    requires (countOf<E>() <= 8 * sizeof(UnderlyingTy))
struct EnumBits final {
    UnderlyingTy value = 0;

    static const EnumBits None;

    template<typename... Args> [[nodiscard]]
    static constexpr EnumBits of(E const first, Args const... args) noexcept {
        if constexpr (sizeof...(Args) > 0) {
            return { (static_cast<UnderlyingTy>(1) << indexOf(first)) | of(args...).value };
        } else {
            return { static_cast<UnderlyingTy>(1) << indexOf(first) };
        }
    }

    [[nodiscard]]
    constexpr bool operator[](E const bit) const noexcept {
        return value & (static_cast<UnderlyingTy>(1) << indexOf(bit));
    }

    [[nodiscard]]
    constexpr EnumBits operator|(EnumBits const rhs) const noexcept {
        return EnumBits{ value | rhs.value };
    }

    [[nodiscard]]
    constexpr EnumBits operator|(E const rhs) const noexcept {
        return EnumBits{ value | of(rhs).value };
    }

    [[nodiscard]]
    constexpr void operator|=(EnumBits const rhs) & noexcept {
        value |= rhs.value;
    }

    [[nodiscard]]
    constexpr void operator|=(E const rhs) & noexcept {
        value |= of(rhs).value;
    }
};

template<CountableEnum E, typename UnderlyingTy>
    requires (countOf<E>() <= 8 * sizeof(UnderlyingTy))
constexpr inline EnumBits<E, UnderlyingTy> EnumBits<E, UnderlyingTy>::None{ 0 };

} // namespace core
