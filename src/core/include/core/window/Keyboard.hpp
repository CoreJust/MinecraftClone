#pragma once

#include "Key.hpp"

namespace core {

[[nodiscard]]
bool isKeyPressedWithModifer(Key const key, KeyModifierBit const modifier);
[[nodiscard]]
bool isKeyPressed(Key const key);
[[nodiscard]]
bool isKeyReleased(Key const key);

void keyCallback(void*, int const key, int const scancode, int const action, int const mode);

} // namespace core
