#include "Window.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>

namespace graphics {
namespace {
    void* createWindow(char const* const name, int& width, int& height) {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwWindowHint(GLFW_RED_BITS, mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
        if (width <= 0 || height <= 0) {
            width  = mode->width;
            height = mode->height;
            core::io::info("Setting full-screen window mode: {}x{}", width, height);
        }
        GLFWwindow* window = glfwCreateWindow(width, height, name, monitor, nullptr);
        return reinterpret_cast<void*>(window);
    }
} // namespace


    Window::Window(char const* const name, int width, int height)
        : m_window(createWindow(name, width, height))
        , m_name(name) {
        core::io::info("Created window (title: {}, size: ({} x {}))", name, width, height);
    }

    Window::~Window() {
        if (m_window != nullptr) {
            glfwDestroyWindow(reinterpret_cast<GLFWwindow*>(m_window));
            glfwTerminate();
            m_window = nullptr;
            core::io::info("The window was closed");
        }
    }

    core::memory::UniquePtr<vulkan::VulkanManager> Window::createVulkanManager(core::common::Version const& appVersion) {
        uint32_t glfwExtensionsCount;
        char const** glfwVkExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionsCount);
        auto result = core::memory::makeUP<vulkan::VulkanManager>(
            m_window,
            reinterpret_cast<void*>(&glfwCreateWindowSurface),
            m_name,
            appVersion,
            glfwVkExtensions,
            glfwExtensionsCount);
        core::io::info("Created Vulkan");
        return result;
    }

    bool Window::nextFrame() {
        ASSERT(m_window != nullptr, "Cannot acquire next frame: no RGFW window found!");
        GLFWwindow* window = reinterpret_cast<GLFWwindow*>(m_window);
        if (glfwWindowShouldClose(window))
            return false;
        glfwPollEvents();
        glfwSwapBuffers(window);
        return true;
    }
} // namespace graphics
