#pragma once
#include <Core/Memory/UniquePtr.hpp>
#include "Window.hpp"

namespace graphics {
    class RenderEngine final {
        Window m_window;
        core::memory::UniquePtr<vulkan::Vulkan> m_vulkan;
        vulkan::RenderPipeline m_pipeline;

    public:
        RenderEngine(char const* const name);

        void run();
    };
} // namespace graphics
