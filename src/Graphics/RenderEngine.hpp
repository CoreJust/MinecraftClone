#pragma once
#include <Core/Container/UniquePtr.hpp>
#include "Window/Window.hpp"
#include "Vulkan/VulkanManager.hpp"
#include "Renderer/Pipelines/VoxelPipeline.hpp"

namespace graphics {
    class RenderEngine final {
        window::Window m_window;
        core::UniquePtr<vulkan::VulkanManager> m_vulkanManager;
        renderer::pipelines::VoxelPipeline m_voxelPipeline;
        renderer::pipelines::VoxelVertices<> m_voxelVertices;

    public:
        RenderEngine(char const* const name, core::Version const& appVersion);

        void run();
    };
} // namespace graphics
