#include "Mouse.hpp"
#include <atomic>
#include <Core/Common/Int.hpp>

namespace graphics::window {
namespace {
    std::atomic_bool g_mouseKeyStates[static_cast<usize>(MouseKey::MouseKeysCount)];
    std::atomic<KeyModifierBit> g_mouseKeyModifiers[static_cast<usize>(MouseKey::MouseKeysCount)];
    std::atomic<double> g_wheelX = 0.0;
    std::atomic<double> g_wheelY = 0.0;
    std::atomic<double> g_wheelPrevX = 0.0;
    std::atomic<double> g_wheelPrevY = 0.0;
    std::atomic<double> g_mouseX = 0.0;
    std::atomic<double> g_mouseY = 0.0;
    std::atomic<double> g_mousePrevX = 0.0;
    std::atomic<double> g_mousePrevY = 0.0;
} // namespace

    core::Vec2d getMousePosition() noexcept {
        return { g_mouseX, g_mouseY };
    }

    core::Vec2d getMouseDelta() noexcept {
        return { g_mouseX - g_mousePrevX, g_mouseY - g_mousePrevY };
    }

    core::Vec2d getWheelPosition() noexcept {
        return { g_wheelX, g_wheelY };
    }

    core::Vec2d getWheelDelta() noexcept {
        return { g_wheelX - g_wheelPrevX, g_wheelY - g_wheelPrevY };
    }

    bool isMouseKeyPressedWithModifer(MouseKey key, KeyModifierBit modifier) {
        return g_mouseKeyStates[static_cast<usize>(key)] && (g_mouseKeyModifiers[static_cast<usize>(key)] & modifier) == modifier;
    }

    bool isMouseKeyPressed(MouseKey key) {
        return g_mouseKeyStates[static_cast<usize>(key)];
    }

    bool isMouseKeyReleased(MouseKey key) {
        return g_mouseKeyStates[static_cast<usize>(key)];
    }

    void resetMouseDeltas() {
        g_mousePrevX = g_mouseX.load();
        g_mousePrevY = g_mouseY.load();
        g_wheelPrevX = g_wheelX.load();
        g_wheelPrevY = g_wheelY.load();
    }

    void mousePositionCallback(void*, double x, double y) {
        g_mouseX = x;
        g_mouseY = y;
    }

    void scrollCallback(void*, double xoffset, double yoffset) {
        g_wheelX += xoffset;
        g_wheelY += yoffset;
    }

    void mouseKeyCallback(void*, int const button, int const action, int const mode) {
        g_mouseKeyStates[button] = bool(action);
        g_mouseKeyModifiers[button] = static_cast<KeyModifierBit>(mode);
    }
} // namespace graphics::window
