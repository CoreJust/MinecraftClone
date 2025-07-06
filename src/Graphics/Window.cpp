#include "Window.hpp"
#pragma warning(push)
#pragma warning(disable : 4244)
#define RGFW_IMPLEMENTATION
#define RGFW_VULKAN
#define RGFW_PRINT_ERRORS
#define RGFW_NO_API
#include "RGFW.h"
#pragma warning(pop)
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>

namespace graphics {
    Window::Window(char const* const name, uint32_t width, uint32_t height)
        : m_rgfwWindow(RGFW_createWindow(name, RGFW_RECT(0, 0, width, height), RGFW_windowCenter)) {
        core::io::info("Created window (title: {}, size: ({} x {}))", name, width, height);
        size_t rgfwExtensionsCount;
        char const** rgfwVkExtensions = RGFW_getVKRequiredInstanceExtensions(&rgfwExtensionsCount);
        m_vulkan = std::make_unique<vulkan::Vulkan>(name, rgfwVkExtensions, static_cast<uint32_t>(rgfwExtensionsCount));
        core::io::info("Window is ready for usage");
    }

    Window::~Window() {
        if (m_rgfwWindow != nullptr) {
            RGFW_window_close(reinterpret_cast<RGFW_window*>(m_rgfwWindow));
            m_rgfwWindow = nullptr;
            core::io::info("The window was closed");
        }
    }

    bool Window::nextFrame() {
        ASSERT(m_rgfwWindow != nullptr, "Cannot acquire next frame: no RGFW window found!");
        RGFW_window* window = reinterpret_cast<RGFW_window*>(m_rgfwWindow);
        if (RGFW_window_shouldClose(window))
            return false;
        while (RGFW_window_checkEvent(window)) {
            RGFW_event const& e = window->event;
            if (e.type == RGFW_quit || (e.type == RGFW_keyReleased && e.key == RGFW_escape)) {
                core::io::info("Quitting the window...");
                return false;
            }
        }
        RGFW_window_swapBuffers(window);
        return true;
    }
} // namespace graphics
