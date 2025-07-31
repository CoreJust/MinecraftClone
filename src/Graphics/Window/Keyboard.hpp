#pragma once
#include "Key.hpp"

namespace graphics::window {
    bool isKeyPressedWithModifer(Key key, KeyModifierBit modifier);
    bool isKeyPressed(Key key);
    bool isKeyReleased(Key key);

    void keyCallback(void*, int const key, int const scancode, int const action, int const mode);
} // namespace graphics::window
