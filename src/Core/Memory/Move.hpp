#pragma once
#include <Core/Meta/UnRef.hpp>

namespace core {
    template<typename T>
    [[nodiscard]] constexpr UnRef<T>&& move(T&& x) noexcept {
        return static_cast<UnRef<T>&&>(x);
    }
} // namespace core
