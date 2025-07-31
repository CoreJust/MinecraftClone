#pragma once
#include <Core/Macro/Attributes.hpp>
#include "Key.hpp"

namespace graphics::window {
    PURE bool isKeyPressedWithModifer(Key key, KeyModifierBit modifier);
    PURE bool isKeyPressed(Key key);
    PURE bool isKeyReleased(Key key);

    void keyCallback(void*, int const key, int const scancode, int const action, int const mode);
} // namespace graphics::window
