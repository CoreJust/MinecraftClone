#pragma once
#include <Core/Container/UniquePtr.hpp>
#include <Engine/Camera.hpp>
#include "Window/Window.hpp"
#include "Vulkan/VulkanManager.hpp"
#include "Renderer/Pipelines/VoxelPipeline.hpp"

namespace graphics {
    class RenderEngine final {
        window::Window m_window;
        core::UniquePtr<vulkan::VulkanManager> m_vulkanManager;
        engine::Camera& m_pCamera;
        renderer::pipelines::VoxelPipeline m_voxelPipeline;
        renderer::pipelines::VoxelVertices<> m_voxelVertices;

    public:
        RenderEngine(char const* const name, core::Version const& appVersion, engine::Camera& camera);

        void run();
    };
} // namespace graphics
