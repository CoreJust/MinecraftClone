#include <core/graphics/Window.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/Log.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <utility>

namespace core {

Window::Window(int const width, int const height, std::string const& title) {
    ASSERT(glfwInit(), "glfwInit failed");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (m_window == nullptr) {
        glfwTerminate();
        ASSERT(false, "glfwCreateWindow failed");
    }
}

Window::~Window() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

bool Window::shouldClose() const noexcept {
    return glfwWindowShouldClose(m_window) != 0;
}

void Window::pollEvents() const noexcept {
    glfwPollEvents();
}

bool Window::isKeyPressed(Key const key) const noexcept {
    return glfwGetKey(m_window, static_cast<int>(key)) == GLFW_PRESS;
}

std::pair<std::uint32_t, std::uint32_t> Window::framebufferSize() const noexcept {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return {
        static_cast<std::uint32_t>(width > 0 ? width : 1),
        static_cast<std::uint32_t>(height > 0 ? height : 1),
    };
}

bool Window::isFramebufferSizeZero() const noexcept {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return width == 0 || height == 0;
}

} // namespace core
