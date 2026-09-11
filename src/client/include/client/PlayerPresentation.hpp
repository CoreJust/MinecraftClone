#pragma once

#include "Camera.hpp"

#include <shared/world/World.hpp>

#include <glm/vec3.hpp>

namespace client {

[[nodiscard]]
glm::dvec3 localPlayerCenterPosition(shared::Player const& player) noexcept;

[[nodiscard]]
CameraPose localPlayerThirdPersonPose(
    shared::Player const& player,
    CameraAngles angles
) noexcept;

[[nodiscard]]
bool shouldRenderRemotePlayer(shared::Player const& player, char local_character) noexcept;

} // namespace client
