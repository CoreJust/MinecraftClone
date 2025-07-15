#include "Keyboard.hpp"
#include <GLFW/glfw3.h>
#include <Core/IO/Logger.hpp>

namespace graphics::window {
    void keyCallback(void*, int const key, int const scancode, int const action, int const mode) {
        core::trace("Key {} (scancode {}); action {}, mode {}", key, scancode, action, mode);
    }
} // namespace graphics::window
