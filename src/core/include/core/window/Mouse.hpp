#pragma once

#include "Key.hpp"

#include <glm/glm.hpp>

namespace core {

[[nodiscard]]
glm::vec2 getMousePosition() noexcept;
[[nodiscard]]
glm::vec2 getMouseDelta() noexcept;

[[nodiscard]]
glm::vec2 getWheelPosition() noexcept;
[[nodiscard]]
glm::vec2 getWheelDelta() noexcept;

[[nodiscard]]
bool isMouseKeyPressedWithModifer(MouseKey const key, KeyModifierBit const modifier);
[[nodiscard]]
bool isMouseKeyPressed(MouseKey const key);
[[nodiscard]]
bool isMouseKeyReleased(MouseKey const key);

void resetMouseDeltas();
void mousePositionCallback(void*, double const x, double const y);
void scrollCallback(void*, double const xoffset, double const yoffset);
void mouseKeyCallback(void*, int const button, int const action, int const mode);

} // namespace core
