#include <core/window/Window.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/Log.hpp>
#include <core/window/Keyboard.hpp>
#include <core/window/Mouse.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <utility>

namespace core {
namespace {

void errorCallback(int const code, char const* const description) {
    MC_ERROR("GLFW error (code {}): {}", code, description);
}

GLFWwindow* createWindow(char const* const name, int width, int height) {
    if (!glfwInit()) {
        MC_CRITICAL("Failed to initialize GLFW");
        throw WindowException{ };
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
    if (width <= 0 || height <= 0) {
        width = mode->width;
        height = mode->height;
        MC_INFO("Setting full-screen window mode: {}x{}", width, height);
        window = glfwCreateWindow(width, height, name, monitor, nullptr);
    } else {
        MC_INFO("Setting window mode: {}x{}", width, height);
        window = glfwCreateWindow(width, height, name, nullptr, nullptr);
    }
    if (!window) {
        MC_CRITICAL("Failed to create a window");
        throw WindowException { };
    }
    glfwSetKeyCallback(window, reinterpret_cast<GLFWkeyfun>(keyCallback));
    glfwSetMouseButtonCallback(window, reinterpret_cast<GLFWmousebuttonfun>(mouseKeyCallback));
    glfwSetCursorPosCallback(window, reinterpret_cast<GLFWcursorposfun>(mousePositionCallback));
    glfwSetScrollCallback(window, reinterpret_cast<GLFWscrollfun>(scrollCallback));
    MC_INFO(
        "Created window:\n\t"   \
        "Title:      {}\n\t"    \
        "Size:       {}x{}\n\t" \
        "Red bits:   {}\n\t"    \
        "Green bits: {}\n\t"    \
        "Blue bits:  {}\n\t"    \
        "Refresh rate: {}",
        name,
        width, height,
        mode->redBits,
        mode->greenBits,
        mode->blueBits,
        mode->refreshRate);
    return window;
}

} // namespace

Window::Window(std::string title, int const width, int const height) 
    : m_window(createWindow(title.data(), width, height))
    , m_title(std::move(title))
{
    glfwSetWindowUserPointer(m_window, this);
}

Window::~Window() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        MC_DEBUG("Window was closed");
    }
    glfwTerminate();
}

bool Window::nextFrame() const noexcept {
    ASSERT(m_window != nullptr, "Cannot acquire next frame: no GLFW window found!");
    if (glfwWindowShouldClose(m_window)) {
        return false;
    }
    glfwPollEvents();
    return true;
}

void Window::enableCursor(bool value) {
    ASSERT(m_window != nullptr, "Cannot set cursor mode: no GLFW window found!");
    if (m_cursor_enabled == value) {
        return;
    }
    glfwSetInputMode(m_window, GLFW_CURSOR, value ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    m_cursor_enabled = value;
}

std::pair<uint32_t, uint32_t> Window::framebufferSize() const noexcept {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return {
        static_cast<uint32_t>(width > 0 ? width : 1),
        static_cast<uint32_t>(height > 0 ? height : 1),
    };
}

bool Window::isFramebufferSizeZero() const noexcept {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return width == 0 || height == 0;
}

void Window::framebuffersResized(GLFWwindow* window, int width, int height) {
    Window& self = *reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self.m_ignore_minimized && width == 0 && height == 0) {
        do {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        } while (width == 0 && height == 0);
    } else if (self.m_resize_callback) {
        self.m_resize_callback(width, height);
    }
}

} // namespace core
