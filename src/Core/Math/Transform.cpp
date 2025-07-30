#include "Transform.hpp"
#include <cmath>

namespace core {
    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::rotation(Rotation r) noexcept {
        if constexpr (Dims == 2) {
            T sinTheta = std::sin(r.data[0]);
            T cosTheta = std::cos(r.data[0]);
            return {
                cosTheta,          -sinTheta,         static_cast<T>(0),
                sinTheta,           cosTheta,         static_cast<T>(0),
                static_cast<T>(0), static_cast<T>(0), static_cast<T>(1),
            };
        } else {
            T sinA = std::sin(r.data[0]);
            T cosA = std::cos(r.data[0]);
            T sinB = std::sin(r.data[1]);
            T cosB = std::cos(r.data[1]);
            T sinY = std::sin(r.data[2]);
            T cosY = std::cos(r.data[2]);
            return {
                cosA * cosB,       cosA * sinB * sinY - sinA * cosY, cosA * sinB * cosY + sinA * sinY,        static_cast<T>(0),
                sinA * cosB,       sinA * sinB * sinY + cosA * cosY, sinA * sinB * cosY - cosA * sinY,        static_cast<T>(0),
                -sinB,             cosB * sinY,                      cosB * cosY,                             static_cast<T>(0),
                static_cast<T>(0), static_cast<T>(0),                static_cast<T>(0),                       static_cast<T>(1),
            };
        }
    }

#define INSTANTIATE_TRANSFORM(dims, type) template Transform<dims, type> Transform<dims, type>::rotation(Transform<dims, type>::Rotation r) noexcept
    INSTANTIATE_TRANSFORM(2, float);
    INSTANTIATE_TRANSFORM(2, double);
    INSTANTIATE_TRANSFORM(3, float);
    INSTANTIATE_TRANSFORM(3, double);
#undef INSTANTIATE_TRANSFORM
} // namespace core
