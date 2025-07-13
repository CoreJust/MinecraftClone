#pragma once
#include <Core/Memory/UniquePtr.hpp>
#include "Vulkan/VulkanManager.hpp"

namespace graphics {
    class Window final {
        void* m_window;
        char const* const m_name;

    public:
        Window(char const* const name, int width = 0, int height = 0);
        ~Window();

        core::memory::UniquePtr<vulkan::VulkanManager> createVulkanManager(core::common::Version const& appVersion);
        bool nextFrame();
    };
} // namespace graphics
