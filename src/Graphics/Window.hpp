#pragma once
#include <cstdint>
#include <memory>
#include "Vulkan/Vulkan.hpp"

namespace graphics {
    class Window final {
        void* m_rgfwWindow;
        std::unique_ptr<vulkan::Vulkan> m_vulkan;

    public:
        Window(char const* const name, uint32_t width, uint32_t height);
        ~Window();

        bool nextFrame();
    };
} // namespace graphics
