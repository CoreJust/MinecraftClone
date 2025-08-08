#pragma once
#include <Graphics/Vulkan/Pipeline/Vertex.hpp>
#include <Graphics/Vulkan/Pipeline/RenderPipeline.hpp>
#include <Graphics/Vulkan/Pipeline/PipelineOptions.hpp>
#include <Graphics/Vulkan/Pipeline/Vertices.hpp>

namespace graphics::renderer::pipelines {
    namespace vp = vulkan::pipeline;
    struct VoxelVertex final {
        const static inline vp::Attribute VertexLayout[] = {
            vp::Attribute::of("vec3f32"),
            vp::Attribute::of("vec2f32"),
        };

        core::Vec3f position;
        core::Vec2f texCoords;
    };

    class VoxelPipeline final : public vp::RenderPipeline {
        const static inline vp::Attribute VertexPushConstants[] = {
            vp::Attribute::of("mat4f32"),
        };

        const static inline vp::Descriptor Descriptors[] = {
            vp::Descriptor { .type = vp::DescriptorType::Sampler, .stages = vp::ShaderStageBit::Fragment },
        };

    public:
        using Vertex = VoxelVertex;
        static inline vp::PipelineOptions Options {
            .vertexShaderPath   = "voxel.vert",
            .fragmentShaderPath = "voxel.frag",
            .attributes         = Vertex::VertexLayout,
            .vertexPushContants = VertexPushConstants,
            .descriptors        = Descriptors,
        };

    public:
        VoxelPipeline(usize index) : vp::RenderPipeline(index) { }
    };

    template<typename Index = u16>
    using VoxelVertices = vp::Vertices<VoxelVertex, Index>;
} // namespace graphics::renderer::pipelines
