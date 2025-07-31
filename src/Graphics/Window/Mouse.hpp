#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Math/Vec.hpp>
#include "Key.hpp"

namespace graphics::window {
    PURE core::Vec2d getMousePosition() noexcept;
    PURE core::Vec2d getMouseDelta() noexcept;

    PURE core::Vec2d getWheelPosition() noexcept;
    PURE core::Vec2d getWheelDelta() noexcept;

    PURE bool isMouseKeyPressedWithModifer(MouseKey key, KeyModifierBit modifier);
    PURE bool isMouseKeyPressed(MouseKey key);
    PURE bool isMouseKeyReleased(MouseKey key);

    void resetMouseDeltas();
    void mousePositionCallback(void*, double x, double y);
    void scrollCallback(void*, double xoffset, double yoffset);
    void mouseKeyCallback(void*, int const button, int const action, int const mode);
} // namespace graphics::window
