#pragma once
#include <cstdint>
#include <Core/Memory/UniquePtr.hpp>
#include "Vulkan/Vulkan.hpp"

namespace graphics {
    class Window final {
        void* m_rgfwWindow;
        core::memory::UniquePtr<vulkan::Vulkan> m_vulkan;

    public:
        Window(char const* const name, uint32_t width, uint32_t height);
        ~Window();

        bool nextFrame();
    };
} // namespace graphics
