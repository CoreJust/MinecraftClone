#include <core/window/Keyboard.hpp>

#include <atomic>

namespace core {

namespace {
std::atomic_bool g_key_states[countOf<Key>()];
std::atomic<KeyModifierBit> g_key_modifiers[countOf<Key>()];
} // namespace

bool isKeyPressedWithModifer(Key const key, KeyModifierBit const modifier) {
    return g_key_states[indexOf(key)] && (g_key_modifiers[indexOf(key)] & modifier) == modifier;
}

bool isKeyPressed(Key const key) {
    return g_key_states[indexOf(key)];
}

bool isKeyReleased(Key const key) {
    return !g_key_states[indexOf(key)];
}

void keyCallback(void*, int const key, [[maybe_unused]] int const scancode, int const action, int const mode) {
    if (key < 0 || static_cast<size_t>(key) >= countOf<Key>()) {
        return;
    }
    g_key_states[key] = bool(action);
    g_key_modifiers[key] = static_cast<KeyModifierBit>(mode);
}

} // namespace core
