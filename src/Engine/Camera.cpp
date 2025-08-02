#include "Camera.hpp"
#include <mutex>
#include <Core/Memory/TypeErasedAccessor.hpp>

namespace engine {
namespace {
    std::lock_guard<std::mutex> lock(void*& mutex) {
        return std::lock_guard(core::TypeErasedAccessor<std::mutex>::get(mutex));
    }
} // namespace

#define WITH_LOCK(l) auto lock__ = lock(l)

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
        , m_lock(core::TypeErasedAccessor<std::mutex>::make())
        , m_isOrthogonal(isOrthogonal)
    {
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
    }

    Camera::~Camera() {
        core::TypeErasedAccessor<std::mutex>::destroy(m_lock);
    }

    Camera& Camera::setPosition(core::Vec3d const& position) &noexcept {
        WITH_LOCK(m_lock);
        m_position = position;
        m_projectionView = m_projection * makeView();
        return *this;
    }

    Camera& Camera::setRotation(core::Vec3d const& rotation) &noexcept {
        WITH_LOCK(m_lock);
        m_rotation = rotation;
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::move(core::Vec3d const& shift) &noexcept {
        WITH_LOCK(m_lock);
        m_position += shift;
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::rotate(core::Vec3d const& shift) &noexcept {
        WITH_LOCK(m_lock);
        m_rotation += shift;
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::setFoV(double fov) &noexcept {
        WITH_LOCK(m_lock);
        m_fov = fov;
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::setAspectRatio(double aspectRatio) &noexcept {
        WITH_LOCK(m_lock);
        m_aspectRatio = aspectRatio;
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
        return *this;
    }
    
    Camera& Camera::setOrthogonal(bool mode) &noexcept {
        WITH_LOCK(m_lock);
        m_isOrthogonal = mode;
        m_projection = makeProjection();
        m_projectionView = m_projection * makeView();
        return *this;
    }

    core::Transform3d const& Camera::projectionView() const noexcept {
        WITH_LOCK(m_lock);
        return m_projectionView;
    }

    core::Vec3d const& Camera::position() const noexcept {
        WITH_LOCK(m_lock);
        return m_position;
    }

    core::Vec3d const& Camera::rotation() const noexcept {
        WITH_LOCK(m_lock);
        return m_rotation;
    }

    core::Radiansd Camera::fov() const noexcept {
        WITH_LOCK(m_lock);
        return m_fov;
    }

    double Camera::aspectRatio() const noexcept {
        WITH_LOCK(m_lock);
        return m_aspectRatio;
    }

    bool Camera::isOrthogonal() const noexcept {
        WITH_LOCK(m_lock);
        return m_isOrthogonal;
    }

    core::Transform3d Camera::makeProjection() const noexcept {
        return m_isOrthogonal
            ? core::Transform3d::vulkanClip() * core::Transform3d::orthogonal(m_aspectRatio, m_fov)
            : core::Transform3d::vulkanClip() * core::Transform3d::perspective(m_aspectRatio, m_fov);
    }

    core::Transform3d Camera::makeView() const noexcept {
        return core::Transform3d::view(m_position, m_rotation);
    }
} // namespace engine
