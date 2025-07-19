#pragma once
#include <Core/Memory/UniquePtr.hpp>
#include "Window/Window.hpp"
#include "Vulkan/VulkanManager.hpp"

namespace graphics {
    class RenderEngine final {
        window::Window m_window;
        core::UniquePtr<vulkan::VulkanManager> m_vulkanManager;
        vulkan::pipeline::RenderPipeline m_pipeline;

    public:
        RenderEngine(char const* const name, core::Version const& appVersion);

        void run();
    };
} // namespace graphics
