#pragma once
#include <Core/Memory/UniquePtr.hpp>
#include "Window.hpp"

namespace graphics {
    class RenderEngine final {
        Window m_window;
        core::memory::UniquePtr<vulkan::VulkanManager> m_vulkanManager;
        vulkan::RenderPipeline m_pipeline;

    public:
        RenderEngine(char const* const name, core::common::Version const& appVersion);

        void run();
    };
} // namespace graphics
