#pragma once
#include <Graphics/Vulkan/Pipeline/Vertex.hpp>
#include <Graphics/Vulkan/Pipeline/RenderPipeline.hpp>
#include <Graphics/Vulkan/Pipeline/PipelineOptions.hpp>
#include <Graphics/Vulkan/Pipeline/Vertices.hpp>

namespace graphics::renderer::pipelines {
    namespace vp = vulkan::pipeline;
    struct VoxelVertex final {
        constexpr static inline vp::Attribute VertexLayout[] = {
            vp::Attribute { .type = vp::Type::Float, .bits = vp::Bits::Bits32, .size = vp::Size::Vec3 },
            vp::Attribute { .type = vp::Type::Float, .bits = vp::Bits::Bits32, .size = vp::Size::Vec2 },
        };

        core::Vec3f position;
        core::Vec2f texCoords;
    };

    class VoxelPipeline final : public vp::RenderPipeline {
        constexpr static inline vp::Attribute VertexPushConstants[] = {
            vp::Attribute { .type = vp::Type::Float, .bits = vp::Bits::Bits32, .size = vp::Size::Mat4 },
        };

    public:
        using Vertex = VoxelVertex;
        static inline vp::PipelineOptions Options {
            .vertexShaderPath   = "voxel.vert",
            .fragmentShaderPath = "voxel.frag",
            .attributes         = Vertex::VertexLayout,
            .vertexPushContants = VertexPushConstants,
        };

    public:
        VoxelPipeline(usize index) : vp::RenderPipeline(index) { }
    };

    template<typename Index = u16>
    using VoxelVertices = vp::Vertices<VoxelVertex, Index>;
} // namespace graphics::renderer::pipelines
