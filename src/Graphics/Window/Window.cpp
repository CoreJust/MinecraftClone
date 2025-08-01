#include "Window.hpp"
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "Keyboard.hpp"
#include "Mouse.hpp"
#include "WindowException.hpp"

namespace graphics::window {
namespace {
    void errorCallback(int const code, char const* const description) {
        core::error("GLFW error (code {}): {}", code, description);
    }

    GLFWwindow* createWindow(char const* const name, core::Vec2<int>& size) {
        if (!glfwInit()) {
            core::fatal("Failed to initialize GLFW");
            throw WindowException { };
        }
        glfwSetErrorCallback(errorCallback);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwWindowHint(GLFW_RED_BITS,     mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS,   mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS,    mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
        GLFWwindow* window = nullptr;
        if (size[0] <= 0 || size[1] <= 0) {
            size = core::Vec2<int> {{ mode->width, mode->height }};
            core::info("Setting full-screen window mode: {}x{}", size[0], size[1]);
            window = glfwCreateWindow(size[0], size[1], name, monitor, nullptr);
        } else {
            core::info("Setting window mode: {}x{}", size[0], size[1]);
            window = glfwCreateWindow(size[0], size[1], name, nullptr, nullptr);
        }
        if (!window) {
            core::fatal("Failed to create a window");
            throw WindowException { };
        }
        glfwSetKeyCallback(window, reinterpret_cast<GLFWkeyfun>(keyCallback));
        glfwSetMouseButtonCallback(window, reinterpret_cast<GLFWmousebuttonfun>(mouseKeyCallback));
        glfwSetCursorPosCallback(window, reinterpret_cast<GLFWcursorposfun>(mousePositionCallback));
        glfwSetScrollCallback(window, reinterpret_cast<GLFWscrollfun>(scrollCallback));
        core::info(
            "Created window:\n\t"   \
            "Title:      {}\n\t"    \
            "Size:       {}x{}\n\t" \
            "Red bits:   {}\n\t"    \
            "Green bits: {}\n\t"    \
            "Blue bits:  {}\n\t"    \
            "Refresh rate: {}",
            name,
            size[0], size[1],
            mode->redBits,
            mode->greenBits,
            mode->blueBits,
            mode->refreshRate);
        return window;
    }
} // namespace


    Window::Window(char const* const name, core::Vec2<int> size)
        : m_window(createWindow(name, size))
        , m_name(name)
    {
        glfwSetWindowUserPointer(m_window, this);
        glfwSetFramebufferSizeCallback(m_window, Window::framebuffersResized);
    }

    Window::~Window() {
        if (m_window != nullptr) {
            glfwDestroyWindow(m_window);
            glfwTerminate();
            m_window = nullptr;
            core::info("The window was closed");
        }
    }

    bool Window::nextFrame() {
        ASSERT(m_window != nullptr, "Cannot acquire next frame: no GLFW window found!");
        if (glfwWindowShouldClose(m_window))
            return false;
        glfwPollEvents();
        return true;
    }
    
    void Window::enableCursor(bool value) {
        ASSERT(m_window != nullptr, "Cannot set cursor mode: no GLFW window found!");
        if (m_cursorEnabled == value)
            return;
        glfwSetInputMode(m_window, GLFW_CURSOR, value ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
        m_cursorEnabled = value;
    }

    void* Window::getSurfaceCreateCallback() const noexcept {
        return reinterpret_cast<void*>(&glfwCreateWindowSurface);
    }
    
    core::ArrayView<char const*> Window::getRequiredExtensions() const {
        u32 glfwExtensionsCount;
        char const** glfwVkExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionsCount);
        return core::ArrayView<char const*> { glfwVkExtensions, glfwExtensionsCount };
    }

    double Window::aspectRatio() const noexcept {
        core::Vec2u32 size = pixelSize();
        return static_cast<double>(size[0]) / static_cast<double>(size[1]);
    }

    core::Vec2u32 Window::pixelSize() const noexcept {
        int width, height;
        glfwGetFramebufferSize(m_window, &width, &height);
        return {{
            width  < 0 ? 0 : static_cast<u32>(width),
            height < 0 ? 0 : static_cast<u32>(height),
        }};
    }

    void Window::framebuffersResized(GLFWwindow* window, int width, int height) {
        Window& self = *reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self.m_ignoreMinimized && width == 0 && height == 0) {
            do {
                glfwGetFramebufferSize(window, &width, &height);
                glfwWaitEvents();
            } while (width == 0 && height == 0);
            return;
        }
        if (self.m_resizeCallback)
            self.m_resizeCallback({{ width, height }});
    }
} // namespace graphics::window
