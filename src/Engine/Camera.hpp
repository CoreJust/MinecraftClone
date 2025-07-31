#pragma once
#include <Core/Math/Transform.hpp>

namespace engine {
    class Camera final {
        core::Transform3d m_projection;
        core::Transform3d m_projectionView;
        core::Vec3d m_position;
        core::Vec3d m_rotation;
        core::Radiansd m_fov;
        double m_aspectRatio;
        mutable void* m_lock = nullptr;
        bool m_isOrthogonal;

    public:
        Camera(
            core::Vec3d const& position = core::Vec3d::zero(),
            core::Vec3d const& rotation = core::Vec3d::zero(),
            core::Radiansd fov = core::RadiansQuarter,
            double aspectRatio = 1.0,
            bool isOrthogonal = false) noexcept;
        ~Camera();

        Camera& setPosition   (core::Vec3d const& position)    &noexcept;
        Camera& setRotation   (core::Vec3d const& rotation)    &noexcept;
        Camera& move          (core::Vec3d const& shift)       &noexcept;
        Camera& rotate        (core::Vec3d const& shift)       &noexcept;
        Camera& setFoV        (core::Radiansd     fov)         &noexcept;
        Camera& setAspectRatio(double             aspectRatio) &noexcept;
        Camera& setOrthogonal (bool               mode)        &noexcept;

        PURE core::Transform3d const& projectionView() const noexcept;

        PURE core::Vec3d const& position    () const noexcept;
        PURE core::Vec3d const& rotation    () const noexcept;
        PURE core::Radiansd     fov         () const noexcept;
        PURE double             aspectRatio () const noexcept;
        PURE bool               isOrthogonal() const noexcept;

    private:
        core::Transform3d makeProjection() const noexcept;
        core::Transform3d makeView      () const noexcept;
    };
} // namespace engine
