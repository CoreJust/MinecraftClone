#pragma once
#include <cmath>
#include <Core/Meta/FloatingPoint.hpp>
#include <Core/Memory/Forward.hpp>
#include "Mat.hpp"

namespace core {
    template<usize Dims_, FloatingPoint T>
    struct Transform : public Mat<Dims_ + 1, Dims_ + 1, T> {
        using Parent = Mat<Dims_ + 1, Dims_ + 1, T>;
        using Parent::operator*;

        static_assert(Dims_ == 2 || Dims_ == 3); // Note: can be extended for higher dimensions, but let it be just 2D and 3D for the time being.
        constexpr static inline usize Dims = Dims_;
        constexpr static inline usize AnglesCount = (Dims * (Dims - 1)) / 2;

        using Translation = Vec<Dims, T>;
        // yaw, pitch, roll - rotations around z, y, and x respectively.
        using Rotation    = Vec<AnglesCount, T>;
        using Scale       = Vec<Dims, T>;
        using Point       = Vec<Dims, T>;

        constexpr Transform(auto&&... args) noexcept : Parent { FORWARD(args)... } { }

        PURE constexpr static Transform translation(Translation t) noexcept {
            Transform result = Parent::identity();
            for (usize i = 0; i < Dims; ++i)
                result.data[i * Parent::Cols + (Parent::Rows - 1)] = t.data[i];
            return result;
        }

        PURE static Transform rotation(Rotation r) noexcept;

        PURE constexpr static Transform scale(Scale s) noexcept { return Parent::diagonal(s.template resized<Dims + 1>(static_cast<T>(1))); }

        PURE constexpr Parent::Row operator*(Point const& rhs) const noexcept { return *this * rhs.template resized<Dims + 1>(static_cast<T>(1)); }
    };

    // A 2D transform
    template<FloatingPoint T>
    using Transform2 = Transform<2, T>;
    // A 3D transform
    template<FloatingPoint T>
    using Transform3 = Transform<3, T>;

    using Transform2f = Transform2<float>;
    using Transform2d = Transform2<double>;
    using Transform3f = Transform3<float>;
    using Transform3d = Transform3<double>;

    
} // namespace core
