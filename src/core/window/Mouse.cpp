#include <core/window/Mouse.hpp>

#include <atomic>

namespace core {
namespace {

std::atomic_bool g_mouse_key_states[static_cast<size_t>(MouseKey::MouseKeysCount)];
std::atomic<KeyModifierBit> g_mouse_key_modifiers[static_cast<size_t>(MouseKey::MouseKeysCount)];
std::atomic<double> g_wheel_x = 0.0;
std::atomic<double> g_wheel_y = 0.0;
std::atomic<double> g_wheelPrev_x = 0.0;
std::atomic<double> g_wheelPrev_y = 0.0;
std::atomic<double> g_wheelDelta_x = 0.0;
std::atomic<double> g_wheelDelta_y = 0.0;
std::atomic<double> g_mouse_x = 0.0;
std::atomic<double> g_mouse_y = 0.0;
std::atomic<double> g_mousePrev_x = 0.0;
std::atomic<double> g_mousePrev_y = 0.0;
std::atomic<double> g_mouseDelta_x = 0.0;
std::atomic<double> g_mouseDelta_y = 0.0;

} // namespace

glm::vec2 getMousePosition() noexcept {
    return { g_mouse_x.load(), g_mouse_y.load() };
}

glm::vec2 getMouseDelta() noexcept {
    return { g_mouseDelta_x.load(), g_mouseDelta_y.load() };
}

glm::vec2 getWheelPosition() noexcept {
    return { g_wheel_x.load(), g_wheel_y.load() };
}

glm::vec2 getWheelDelta() noexcept {
    return { g_wheelDelta_x.load(), g_wheelDelta_y.load() };
}

bool isMouseKeyPressedWithModifer(MouseKey const key, KeyModifierBit const modifier) {
    return g_mouse_key_states[static_cast<size_t>(key)] && (g_mouse_key_modifiers[static_cast<size_t>(key)] & modifier) == modifier;
}

bool isMouseKeyPressed(MouseKey const key) {
    return g_mouse_key_states[static_cast<size_t>(key)];
}

bool isMouseKeyReleased(MouseKey const key) {
    return g_mouse_key_states[static_cast<size_t>(key)];
}

void resetMouseDeltas() {
    g_mouseDelta_x = g_mouse_x - g_mousePrev_x;
    g_mouseDelta_y = g_mouse_y - g_mousePrev_y;
    g_wheelDelta_x = g_wheel_x - g_wheelPrev_x;
    g_wheelDelta_y = g_wheel_y - g_wheelPrev_y;
    g_mousePrev_x = g_mouse_x.load();
    g_mousePrev_y = g_mouse_y.load();
    g_wheelPrev_x = g_wheel_x.load();
    g_wheelPrev_y = g_wheel_y.load();
}

void mousePositionCallback(void*, double const x, double const y) {
    g_mouse_x = x;
    g_mouse_y = y;
}

void scrollCallback(void*, double const xoffset, double const yoffset) {
    g_wheel_x += xoffset;
    g_wheel_y += yoffset;
}

void mouseKeyCallback(void*, int const button, int const action, int const mode) {
    g_mouse_key_states[button] = bool(action);
    g_mouse_key_modifiers[button] = static_cast<KeyModifierBit>(mode);
}

} // namespace core
