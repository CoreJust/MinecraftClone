#include "Keyboard.hpp"
#include <atomic>
#include <Core/Common/Int.hpp>

namespace graphics::window {
namespace {
    std::atomic_bool g_keyStates[static_cast<usize>(Key::KeysCount)];
    std::atomic<KeyModifierBit> g_keyModifiers[static_cast<usize>(Key::KeysCount)];
} // namespace

    bool isKeyPressedWithModifer(Key key, KeyModifierBit modifier) {
        return g_keyStates[static_cast<usize>(key)] && (g_keyModifiers[static_cast<usize>(key)] & modifier) == modifier;
    }

    bool isKeyPressed(Key key) {
        return g_keyStates[static_cast<usize>(key)];
    }

    bool isKeyReleased(Key key) {
        return !g_keyStates[static_cast<usize>(key)];
    }

    void keyCallback(void*, int const key, [[maybe_unused]] int const scancode, int const action, int const mode) {
        g_keyStates[key] = bool(action);
        g_keyModifiers[key] = static_cast<KeyModifierBit>(mode);
    }
} // namespace graphics::window
