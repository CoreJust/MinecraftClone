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
            T sinA = std::sin(r.data[2]);
            T cosA = std::cos(r.data[2]);
            T sinB = std::sin(r.data[1]);
            T cosB = std::cos(r.data[1]);
            T sinY = std::sin(r.data[0]);
            T cosY = std::cos(r.data[0]);
            return {
                cosA * cosB,       cosA * sinB * sinY - sinA * cosY, cosA * sinB * cosY + sinA * sinY,        static_cast<T>(0),
                sinA * cosB,       sinA * sinB * sinY + cosA * cosY, sinA * sinB * cosY - cosA * sinY,        static_cast<T>(0),
                -sinB,             cosB * sinY,                      cosB * cosY,                             static_cast<T>(0),
                static_cast<T>(0), static_cast<T>(0),                static_cast<T>(0),                       static_cast<T>(1),
            };
        }
    }

    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::perspective(T aspectRatio, Radians<T> fov, T zNear, T zFar) noexcept 
        requires(Dims == 3) {
        T tanHalfFovInverse = static_cast<T>(1) / std::tan(fov * static_cast<T>(0.5));
        T depth = zFar - zNear;
        constexpr T Zero = static_cast<T>(0);
        constexpr T One  = static_cast<T>(1);
        return {
            tanHalfFovInverse / aspectRatio,  Zero,              Zero,          Zero,
            Zero,                            -tanHalfFovInverse, Zero,          Zero,
            Zero,                             Zero,              zFar / depth, -(zFar * zNear) / depth,
            Zero,                             Zero,              One,           Zero,
        };
    }

    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::orthogonal(T aspectRatio, T fov, T zNear, T zFar) noexcept 
        requires(Dims == 3) {
        T left   = -aspectRatio * fov, right = aspectRatio * fov;
        T bottom =               -fov, top   =               fov;
        constexpr T Zero = static_cast<T>(0);
        constexpr T One  = static_cast<T>(1);
        constexpr T Two  = static_cast<T>(2);
        return {
            Two / (right - left), Zero,                  Zero,                 -(right + left) / (right - left),
            Zero,                 Two / (top - bottom),  Zero,                 -(top + bottom) / (top  -bottom),
            Zero,                 Zero,                 -Two / (zFar - zNear), -(zFar + zNear) / (zFar - zNear),
            Zero,                 Zero,                  Zero,                  One,
        };
    }

#define INSTANTIATE_ROTATION(dims, type) template Transform<dims, type> Transform<dims, type>::rotation(Transform<dims, type>::Rotation r) noexcept
#define INSTANTIATE_PERSPECTIVE(type) template Transform<3, type> Transform<3, type>::perspective(type aspectRatio, Radians<type> fov, type zNear, type zFar) noexcept
#define INSTANTIATE_ORTHOGONAL(type) template Transform<3, type> Transform<3, type>::orthogonal(type aspectRatio, type fov, type zNear, type zFar) noexcept
    INSTANTIATE_ROTATION(2, float);
    INSTANTIATE_ROTATION(2, double);
    INSTANTIATE_ROTATION(3, float);
    INSTANTIATE_ROTATION(3, double);
    INSTANTIATE_PERSPECTIVE(float);
    INSTANTIATE_PERSPECTIVE(double);
    INSTANTIATE_ORTHOGONAL(float);
    INSTANTIATE_ORTHOGONAL(double);
#undef INSTANTIATE_ORTHOGONAL
#undef INSTANTIATE_PERSPECTIVE
#undef INSTANTIATE_ROTATION
} // namespace core
