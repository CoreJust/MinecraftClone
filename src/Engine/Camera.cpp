#include "Camera.hpp"

namespace engine {
    Camera::Camera(
        core::Vec3d const& position,
        core::Vec3d const& rotation,
        double fov,
        double aspectRatio,
        bool isOrthogonal) noexcept 
        : m_position(position)
        , m_rotation(rotation)
        , m_fov(fov)
        , m_aspectRatio(aspectRatio)
        , m_isOrthogonal(isOrthogonal)
    {
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
    }

    Camera& Camera::setPosition(core::Vec3d const& position) &noexcept {
        m_position = position;
        m_projectionView = m_projection * makeView();
        return *this;
    }

    Camera& Camera::setRotation(core::Vec3d const& rotation) &noexcept {
        m_rotation = rotation;
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::move(core::Vec3d const& shift) &noexcept {
        m_position += shift;
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::rotate(core::Vec3d const& shift) &noexcept {
        m_rotation += shift;
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::setFoV(double fov) &noexcept {
        m_fov = fov;
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::setAspectRatio(double aspectRatio) &noexcept {
        m_aspectRatio = aspectRatio;
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::setOrthogonal(bool mode) &noexcept {
        m_isOrthogonal = mode;
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
        return *this;
    }

    core::Transform3d Camera::makeProjection() const noexcept {
        return m_isOrthogonal
            ? core::Transform3d::orthogonal(m_aspectRatio, m_fov)
            : core::Transform3d::perspective(m_aspectRatio, m_fov);
    }

    core::Transform3d Camera::makeView() const noexcept {
        return core::Transform3d::translation(-m_position) * core::Transform3d::rotation(m_rotation);
    }
} // namespace engine
