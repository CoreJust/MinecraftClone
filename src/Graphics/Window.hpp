#pragma once
#include <cstdint>
#include "Vulkan/Instance.hpp"

namespace graphics {
    class Window final {
        void* m_rgfwWindow;
        vulkan::Instance m_vkInstance;
    public:
        Window(char const* const name, uint32_t width, uint32_t height);
        ~Window();

        bool nextFrame();
    };
} // namespace graphics
