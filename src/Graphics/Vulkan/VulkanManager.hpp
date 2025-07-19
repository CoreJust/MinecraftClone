#pragma once
#include <vector>
#include <Core/Common/Int.hpp>
#include <Core/Common/Version.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Memory/UniquePtr.hpp>
#include <Core/Collection/DynArray.hpp>
#include "Pipeline/RenderPipeline.hpp"
#include "Pipeline/PipelineOptions.hpp"

namespace graphics::window {
    class Window;
}

namespace graphics::vulkan {
    struct DeviceLostException final { };

    namespace internal {
        class Vulkan;
        class Queue;
        class CommandPool;
        class CommandBuffer;
        class Pipeline;
        class Frame;
    }

    class VulkanManager final {
        struct PipelineNote final {
            pipeline::PipelineOptions options;
            core::UniquePtr<internal::Pipeline> pipeline;
        };

        struct StateSnapshot final {
            core::Version appVersion;
            std::vector<pipeline::PipelineOptions> pipelineOptions;
        };

        core::Version m_appVersion;
        core::UniquePtr<internal::Vulkan> m_vulkan;
        std::vector<PipelineNote> m_pipelines;
        window::Window& m_pWindow;
        internal::Frame* m_frame = nullptr;
        bool m_requiresSwapchainRecreation = false;
        bool m_wantsSwapchainRecreation = false;

    public:
        explicit VulkanManager(StateSnapshot snapshot, window::Window& win);
        VulkanManager(window::Window& win, core::Version const& appVersion);
        VulkanManager(VulkanManager&&) noexcept = delete;
        VulkanManager(VulkanManager const&) noexcept = delete;
        VulkanManager& operator=(VulkanManager&&) noexcept = delete;
        VulkanManager& operator=(VulkanManager const&) noexcept = delete;
        ~VulkanManager();

        StateSnapshot makeSnapshot();
        void requestSwapchainRecreation() noexcept { m_requiresSwapchainRecreation = true; }

        bool startFrame();
        void endFrame();

        PURE pipeline::RenderPipeline createPipeline(pipeline::PipelineOptions options);
        void beginRendering(pipeline::RenderPipeline& pipeline);
        void endRendering(pipeline::RenderPipeline& pipeline);

    private:
        void onSwapchainRecreationRequest();
        void createPipelines();
    };
} // namespace graphics::vulkan
