#pragma once
#include "Entity.hpp"

namespace engine::entity {
    class Player final : public Entity {
    public:
        Player(core::Vec3d const& position);

        void update(core::Duration delta) override;
    };
} // namespace engine::entity
