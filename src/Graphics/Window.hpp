#pragma once
#include <cstdint>
#include <Core/Memory/UniquePtr.hpp>
#include "Vulkan/Vulkan.hpp"

namespace graphics {
    class Window final {
        void* m_rgfwWindow;
        char const* const m_name;

    public:
        Window(char const* const name, uint32_t width, uint32_t height);
        ~Window();

        core::memory::UniquePtr<vulkan::Vulkan> createVulkan();
        bool nextFrame();
    };
} // namespace graphics
