#include "Transform.hpp"
#include <cmath>

namespace core {
    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::rotation(Rotation r) noexcept {
        if constexpr (Dims == 2) {
            ASSERT(!std::isinf(r.data[0]) && !std::isnan(r.data[0]));
            T const sinTheta = std::sin(r.data[0]);
            T const cosTheta = std::cos(r.data[0]);
            return {
                cosTheta,          -sinTheta,         Zero,
                sinTheta,           cosTheta,         Zero,
                Zero,               Zero,             One,
            };
        } else {
            ASSERT(
                   !std::isinf(r.roll()) && !std::isnan(r.roll())
                && !std::isinf(r.pitch()) && !std::isnan(r.pitch())
                && !std::isinf(r.yaw()) && !std::isnan(r.yaw()));
            T const sinA = std::sin(r.roll());
            T const cosA = std::cos(r.roll());
            T const sinB = std::sin(r.pitch());
            T const cosB = std::cos(r.pitch());
            T const sinY = std::sin(r.yaw());
            T const cosY = std::cos(r.yaw());
            return {
                cosA * cosB,       cosA * sinB * sinY - sinA * cosY, cosA * sinB * cosY + sinA * sinY,  Zero,
                sinA * cosB,       sinA * sinB * sinY + cosA * cosY, sinA * sinB * cosY - cosA * sinY,  Zero,
                -sinB,             cosB * sinY,                      cosB * cosY,                       Zero,
                Zero,              Zero,                             Zero,                              One,
            };
        }
    }
    
    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::axisRotation(Radians<T> angle, Vector const& axis) noexcept
        requires(Dims == 3) {
        ASSERT(!std::isinf(angle) && !std::isnan(angle));
        
        Transform const W {
             Zero,     -axis.z(),  axis.y(), Zero,
             axis.z(),  Zero,     -axis.x(), Zero,
            -axis.y(),  axis.x(),  Zero,     Zero,
             Zero,      Zero,      Zero,     One,
        };
        T const sinHalfAngle = std::sin(Half * angle);
        return Parent::identity() + W * std::sin(angle) + W * W * (Two * sinHalfAngle * sinHalfAngle);
    }
    
    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::roll(Radians<T> angle) noexcept requires(Dims == 3) {
        ASSERT(!std::isinf(angle) && !std::isnan(angle));
        
        T const sinTheta = std::sin(angle);
        T const cosTheta = std::cos(angle);
        return {
            One,  Zero,      Zero,     Zero,
            Zero, cosTheta, -sinTheta, Zero,
            Zero, sinTheta,  cosTheta, Zero,
            Zero, Zero,      Zero,     One,
        };
    }

    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::pitch(Radians<T> angle) noexcept requires(Dims == 3) {
        ASSERT(!std::isinf(angle) && !std::isnan(angle));
        
        T const sinTheta = std::sin(angle);
        T const cosTheta = std::cos(angle);
        return {
             cosTheta, Zero, sinTheta, Zero,
             Zero,     One,  Zero,     Zero,
            -sinTheta, Zero, cosTheta, Zero,
             Zero,     Zero, Zero,     One,
        };
    }

    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::yaw(Radians<T> angle) noexcept requires(Dims == 3) {
        ASSERT(!std::isinf(angle) && !std::isnan(angle));
        
        T const sinTheta = std::sin(angle);
        T const cosTheta = std::cos(angle);
        return {
            cosTheta, -sinTheta, Zero, Zero,
            sinTheta,  cosTheta, Zero, Zero,
            Zero,      Zero,     One,  Zero,
            Zero,      Zero,     Zero, One,
        };
    }
    
    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::lookAt(Point eye, Point forward, Point up) noexcept
        requires(Dims == 3) {
        Point z = (forward - eye).normalized();
        Point x = up.cross(z).normalized();
        Point y = z.cross(x);
        return {
            x.x(), x.y(), x.z(), -x.dot(eye),
            y.x(), y.y(), y.z(), -y.dot(eye),
            z.x(), z.y(), z.z(), -z.dot(eye),
            Zero,  Zero,  Zero,   One,
        };
    }
    
    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::view(Translation pos, Rotation rot) noexcept 
        requires(Dims == 3) {
        Point front = Point {
            std::cos(rot.yaw()) * std::cos(rot.pitch()),
            std::sin(rot.pitch()),
            std::sin(rot.yaw()) * std::cos(rot.pitch()),
        }.normalized();
        Point up { Zero, One, Zero };
        if (rot.roll() != Zero)
            up = (axisRotation(rot.roll(), front) * up).template slice<3>();
        return lookAt(pos, pos + front, up);
    }

    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::perspective(T aspectRatio, Radians<T> fov, T zNear, T zFar) noexcept 
        requires(Dims == 3) {
        ASSERT(std::isnormal(zNear) && std::isnormal(fov) && std::isnormal(aspectRatio) && (std::isnormal(zFar) || std::isinf(zFar)));
        ASSERT(zFar > zNear && fov > 0);

        T const ta = One / std::tan(fov * Half);
        if (std::isinf(zFar)) {
            return {
                ta / aspectRatio, Zero,  Zero,    Zero,
                Zero,             ta,    Zero,    Zero,
                Zero,             Zero, -One,    -Two * zNear,
                Zero,             Zero, -One,     Zero,
            };
        } else {
            T const depth = zFar - zNear;
            return {
                ta / aspectRatio, Zero,  Zero,                    Zero,
                Zero,             ta,    Zero,                    Zero,
                Zero,             Zero, -(zFar + zNear) / depth, -Two * zFar * zNear / depth,
                Zero,             Zero, -One,                     Zero,
            };
        }
    }

    template<usize Dims, FloatingPoint T>
    Transform<Dims, T> Transform<Dims, T>::orthogonal(T aspectRatio, T fov, T zNear, T zFar) noexcept 
        requires(Dims == 3) {
        ASSERT(std::isnormal(zNear) && std::isnormal(fov) && std::isnormal(aspectRatio) && (std::isnormal(zFar) || std::isinf(zFar)));
        ASSERT(zFar > zNear && fov > 0);

        T const left   = -aspectRatio * fov, right = aspectRatio * fov;
        T const bottom =               -fov, top   =               fov;
        if (std::isinf(zFar)) {
            return {
                Two / (right - left), Zero,                 Zero, -(right + left) / (right - left),
                Zero,                 Two / (top - bottom), Zero, -(top + bottom) / (top  -bottom),
                Zero,                 Zero,                 Zero, -One,
                Zero,                 Zero,                 Zero,  One,
            };
        } else {
            return {
                Two / (right - left), Zero,                  Zero,                 -(right + left) / (right - left),
                Zero,                 Two / (top - bottom),  Zero,                 -(top + bottom) / (top  -bottom),
                Zero,                 Zero,                 -Two / (zFar - zNear), -(zFar + zNear) / (zFar - zNear),
                Zero,                 Zero,                  Zero,                  One,
            };
        }
    }

#define INSTANTIATE_DT(dims, type) \
    template Transform<dims, type> Transform<dims, type>::rotation(Transform<dims, type>::Rotation r) noexcept
#define INSTANTIATE_T(type)                                                                                                           \
    template Transform<3, type> Transform<3, type>::axisRotation(                                                                     \
        Radians<type> angle, Transform<3, type>::Vector const& axis) noexcept;                                                        \
    template Transform<3, type> Transform<3, type>::roll(Radians<type> angle) noexcept;                                               \
    template Transform<3, type> Transform<3, type>::pitch(Radians<type> angle) noexcept;                                              \
    template Transform<3, type> Transform<3, type>::yaw(Radians<type> angle) noexcept;                                                \
    template Transform<3, type> Transform<3, type>::lookAt(                                                                           \
        Transform<3, type>::Point eye, Transform<3, type>::Point forward, Transform<3, type>::Point up) noexcept;                     \
    template Transform<3, type> Transform<3, type>::view(                                                                             \
        Transform<3, type>::Translation pos, Transform<3, type>::Rotation rot) noexcept;                                              \
    template Transform<3, type> Transform<3, type>::perspective(type aspectRatio, Radians<type> fov, type zNear, type zFar) noexcept; \
    template Transform<3, type> Transform<3, type>::orthogonal(type aspectRatio, type fov, type zNear, type zFar) noexcept
    INSTANTIATE_DT(2, float);
    INSTANTIATE_DT(2, double);
    INSTANTIATE_DT(3, float);
    INSTANTIATE_DT(3, double);
    INSTANTIATE_T(float);
    INSTANTIATE_T(double);
#undef INSTANTIATE_T
#undef INSTANTIATE_DT
} // namespace core
