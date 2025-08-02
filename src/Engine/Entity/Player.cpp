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
        constexpr static double MOUSE_SPEED = core::deg2rad(0.3);

        double const speed = delta.asSeconds() * PLAYER_SPEED;
        if (isKeyPressed(Key::W)) {
            double const m = isKeyPressed(Key::LeftControl) ? 2.4 : 1.3;
            position.x() -= std::cos(rotation.yaw()) * speed * m;
            position.z() -= std::sin(rotation.yaw()) * speed * m;
        } if (isKeyPressed(Key::S)) {
            position.x() += std::cos(rotation.yaw()) * speed;
            position.z() += std::sin(rotation.yaw()) * speed;
        } if (isKeyPressed(Key::A)) {
            position.x() += std::cos(rotation.yaw() + core::RadiansQuarter) * speed;
            position.z() += std::sin(rotation.yaw() + core::RadiansQuarter) * speed;
        } if (isKeyPressed(Key::D)) {
            position.x() -= std::cos(rotation.yaw() + core::RadiansQuarter) * speed;
            position.z() -= std::sin(rotation.yaw() + core::RadiansQuarter) * speed;
        }

        rotation.slice<2, 1>() += getMouseDelta().swizzled<1, 0>() * MOUSE_SPEED;
        if (rotation.pitch() > core::RadiansQuarter * 0.99) {
            rotation.pitch() = core::RadiansQuarter * 0.99;
        } else if (rotation.pitch() < -core::RadiansQuarter * 0.99) {
            rotation.pitch() = -core::RadiansQuarter * 0.99;
        }
    }
} // namespace engine::entity

