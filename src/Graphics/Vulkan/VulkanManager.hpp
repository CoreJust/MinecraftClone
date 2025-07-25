#pragma once
#include <vector>
#include <Core/Common/Int.hpp>
#include <Core/Common/Version.hpp>
#include <Core/Math/Vec.hpp>
#include <Core/Container/UniquePtr.hpp>
#include <Core/Container/DynArray.hpp>
#include "Internal/Pipeline/ShaderStageBit.hpp"
#include "Pipeline/RenderPipeline.hpp"
#include "Pipeline/PipelineOptions.hpp"
#include "Pipeline/Vertices.hpp"

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
        class Buffer;
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
        core::UniquePtr<internal::CommandPool> m_copyCommandPool;
        window::Window& m_pWindow;
        internal::Frame* m_frame = nullptr;
        usize m_currentPipeline = static_cast<usize>(-1);
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

        template<pipeline::PipelineConcept Pipeline>
        PURE Pipeline createPipeline() {
            return Pipeline { createPipelineImpl(Pipeline::Options) };
        }

        bool beginFrame();
        void endFrame();

        void beginRendering(pipeline::RenderPipeline& pipeline);
        void endRendering(pipeline::RenderPipeline& pipeline);

        void pushConstants(internal::ShaderStageBit stage, core::RawMemory constants);
        void drawVertices(pipeline::VerticesBase& vertices);

        template<pipeline::VertexConcept Vertex>
        pipeline::Vertices<Vertex> createVertexBuffer(usize size) {
            return { createVertexBufferImpl(size * sizeof(Vertex)) };
        }

    private:
        usize createPipelineImpl(pipeline::PipelineOptions const& options);
        pipeline::VerticesBase createVertexBufferImpl(usize size);
        void onSwapchainRecreationRequest();
        void createPipelines();
    };
} // namespace graphics::vulkan
