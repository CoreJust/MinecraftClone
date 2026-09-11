#pragma once

#include <shared/world/World.hpp>

#include <glm/vec3.hpp>

namespace client {

[[nodiscard]]
glm::dvec3 localPlayerEyePosition(shared::Player const& player) noexcept;

[[nodiscard]]
bool shouldRenderRemotePlayer(shared::Player const& player, char local_character) noexcept;

} // namespace client
