#pragma once
#include <Core/Math/Color.hpp>
#include <Graphics/Vulkan/Pipeline/Vertex.hpp>
#include <Graphics/Vulkan/Pipeline/RenderPipeline.hpp>
#include <Graphics/Vulkan/Pipeline/PipelineOptions.hpp>

namespace graphics::renderer::pipelines {
    namespace vp = vulkan::pipeline;
    struct VoxelVertex final {
        constexpr static inline vp::Attribute VertexLayout[] = {
            vp::Attribute { .type = vp::Type::Float, .bits = vp::Bits::Bits32, .size = vp::Size::Vec2 },
            vp::Attribute { .type = vp::Type::Float, .bits = vp::Bits::Bits32, .size = vp::Size::Vec3 },
        };

        core::Vec2f position;
        core::Color3 color;
    };

    class VoxelPipeline final : public vp::RenderPipeline {
    public:
        using Vertex = VoxelVertex;
        static inline vp::PipelineOptions Options {
            .vertexShaderPath   = "voxel.vert",
            .fragmentShaderPath = "voxel.frag",
            .attributes         = Vertex::VertexLayout,
        };

    public:
        VoxelPipeline(usize index) : vp::RenderPipeline(index) { }
    };
} // namespace graphics::renderer::pipelines
