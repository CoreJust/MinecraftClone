#include <core/window/Keyboard.hpp>

#include <atomic>

namespace core {

namespace {
std::atomic_bool g_key_states[static_cast<size_t>(Key::KeysCount)];
std::atomic<KeyModifierBit> g_key_modifiers[static_cast<size_t>(Key::KeysCount)];
} // namespace

bool isKeyPressedWithModifer(Key const key, KeyModifierBit const modifier) {
    return g_key_states[static_cast<size_t>(key)] && (g_key_modifiers[static_cast<size_t>(key)] & modifier) == modifier;
}

bool isKeyPressed(Key const key) {
    return g_key_states[static_cast<size_t>(key)];
}

bool isKeyReleased(Key const key) {
    return !g_key_states[static_cast<size_t>(key)];
}

void keyCallback(void*, int const key, [[maybe_unused]] int const scancode, int const action, int const mode) {
    g_key_states[key] = bool(action);
    g_key_modifiers[key] = static_cast<KeyModifierBit>(mode);
}

} // namespace core
