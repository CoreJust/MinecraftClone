#pragma once
#include <Core/Common/Time.hpp>
#include <Core/Math/Vec.hpp>

namespace engine::entity {
    struct Entity {
        core::Vec3d position;
        core::Vec3d rotation;

        constexpr Entity() noexcept = default;
        constexpr Entity(core::Vec3d const& position, core::Vec3d const& rotation) noexcept 
            : position { position }, rotation { rotation }
        { }
        virtual ~Entity() = default;
        virtual void update(core::Duration delta) = 0;
    };
} // namespace engine::entity
