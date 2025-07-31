#include "Player.hpp"
#include <cmath>
#include <Core/Math/Angles.hpp>
#include <Core/IO/Logger.hpp>
#include <Graphics/Window/Keyboard.hpp>
#include <Graphics/Window/Mouse.hpp>

namespace engine::entity {
    Player::Player(core::Vec3d const& position)
        : Entity { position, { 0.0, 0.0, 0.0 } }
    { }

    void Player::update(core::Duration delta) {
        using namespace graphics::window;

        constexpr static double PLAYER_SPEED = 1.0;
        constexpr static double MOUSE_SPEED = core::deg2rad(1.5);

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

        rotation.slice<2>() -= getMouseDelta().swizzled<1, 0>() * MOUSE_SPEED;
        if (rotation[0] > core::RadiansQuarter) {
            rotation[0] = core::RadiansQuarter;
        } else if (rotation[0] < -core::RadiansQuarter) {
            rotation[0] = -core::RadiansQuarter;
        }
    }
} // namespace engine::entity

