#include "Player.hpp"

namespace engine::entity {
    Player::Player(core::Vec3d const& position)
        : Entity { position, { 0.0, 0.0, 0.0 } }
    { }

    void Player::update(core::Duration delta) {
        (void)delta;
    }
} // namespace engine::entity

