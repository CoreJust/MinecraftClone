#pragma once
#include <Core/Meta/FloatingPoint.hpp>

namespace core {
    template<FloatingPoint T>
    using Degrees = T;
    template<FloatingPoint T>
    using Radians = T;

    using Degreesf = Degrees<float>;
    using Degreesd = Degrees<double>;
    using Radiansf = Radians<float>;
    using Radiansd = Radians<double>;

    constexpr static inline double PI = 3.141592653589793;

    template<FloatingPoint T>
    constexpr Radians<T> deg2rad(Degrees<T> degs) noexcept {
        return degs * static_cast<T>(PI / 180.0);
    }

    template<FloatingPoint T>
    constexpr Degrees<T> rad2deg(Radians<T> rads) noexcept {
        return rads * static_cast<T>(180.0 / PI);
    }

    constexpr static inline Radiansd RadiansHalf    { deg2rad(180.0) };
    constexpr static inline Radiansd RadiansQuarter { deg2rad(90.0) };
} // namespace core
