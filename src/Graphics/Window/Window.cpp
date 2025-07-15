#include "Window.hpp"
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "Keyboard.hpp"
#include "WindowException.hpp"

namespace graphics::window {
namespace {
    void errorCallback(int const code, char const* const description) {
        core::error("GLFW error (code {}): {}", code, description);
    }

    void* createWindow(char const* const name, core::Vec2<int>& size) {
        if (!glfwInit()) {
            core::fatal("Failed to initialize GLFW");
            throw WindowException { };
        }
        glfwSetErrorCallback(errorCallback);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwWindowHint(GLFW_RED_BITS,     mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS,   mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS,    mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
        if (size[0] <= 0 || size[1] <= 0) {
            size = core::Vec2<int> {{ mode->width, mode->height }};
            core::info("Setting full-screen window mode: {}x{}", size[0], size[1]);
        }
        GLFWwindow* window = glfwCreateWindow(size[0], size[1], name, monitor, nullptr);
        if (!window) {
            core::fatal("Failed to create a window");
            throw WindowException { };
        }
        glfwSetKeyCallback(window, reinterpret_cast<GLFWkeyfun>(keyCallback));
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
        return reinterpret_cast<void*>(window);
    }
} // namespace


    Window::Window(char const* const name, core::Vec2<int> size)
        : m_window(createWindow(name, size))
        , m_name(name)
    { }

    Window::~Window() {
        if (m_window != nullptr) {
            glfwDestroyWindow(reinterpret_cast<GLFWwindow*>(m_window));
            glfwTerminate();
            m_window = nullptr;
            core::info("The window was closed");
        }
    }

    bool Window::nextFrame() {
        ASSERT(m_window != nullptr, "Cannot acquire next frame: no GLFW window found!");
        GLFWwindow* window = reinterpret_cast<GLFWwindow*>(m_window);
        if (glfwWindowShouldClose(window))
            return false;
        glfwPollEvents();
        return true;
    }

    void* Window::getSurfaceCreateCallback() const noexcept {
        return reinterpret_cast<void*>(&glfwCreateWindowSurface);
    }
    
    core::ArrayView<char const*> Window::getRequiredExtensions() const {
        u32 glfwExtensionsCount;
        char const** glfwVkExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionsCount);
        return core::ArrayView<char const*> { glfwVkExtensions, glfwExtensionsCount };
    }

    core::Vec2u32 Window::pixelSize() const noexcept {
        int width, height;
        glfwGetFramebufferSize(reinterpret_cast<GLFWwindow*>(m_window), &width, &height);
        return {{
            width  < 0 ? 0 : static_cast<u32>(width),
            height < 0 ? 0 : static_cast<u32>(height),
        }};
    }
} // namespace graphics::window
