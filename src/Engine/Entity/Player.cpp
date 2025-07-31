#include "Player.hpp"
#include <cmath>
#include <Core/Math/Angles.hpp>
#include <Graphics/Window/Keyboard.hpp>

namespace engine::entity {
    Player::Player(core::Vec3d const& position)
        : Entity { position, { 0.0, 0.0, 0.0 } }
    { }

    void Player::update(core::Duration delta) {
        using namespace graphics::window;

        constexpr static double PLAYER_SPEED = 1.0;
        double const speed = delta.asSeconds() * PLAYER_SPEED;
        if (isKeyPressed(Key::W)) {
            double const m = isKeyPressed(Key::LeftControl) ? 2.4 : 1.3;
            position[0] += std::cos(rotation[1] + core::RadiansQuarter) * speed * m;
            position[2] += std::sin(rotation[1] + core::RadiansQuarter) * speed * m;
        } if (isKeyPressed(Key::S)) {
            position[0] -= std::cos(rotation[1] + core::RadiansQuarter) * speed;
            position[2] -= std::sin(rotation[1] + core::RadiansQuarter) * speed;
        } if (isKeyPressed(Key::A)) {
            position[0] -= std::cos(rotation[1]) * speed;
            position[2] -= std::sin(rotation[1]) * speed;
        } if (isKeyPressed(Key::D)) {
            position[0] += std::cos(rotation[1]) * speed;
            position[2] += std::sin(rotation[1]) * speed;
        }
    }
} // namespace engine::entity

