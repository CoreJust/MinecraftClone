#pragma once

#include <core/common/EnumBits.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/GraphicsPipeline.hpp>
#include <core/vulkan/PipelineLayout.hpp>
#include <core/vulkan/ShaderModule.hpp>
#include <core/vulkan/SpirV.hpp>
#include <core/vulkan/enum/BlendFactor.hpp>
#include <core/vulkan/enum/BlendOp.hpp>
#include <core/vulkan/enum/ColorComponent.hpp>
#include <core/vulkan/enum/CompareOp.hpp>
#include <core/vulkan/enum/CullMode.hpp>
#include <core/vulkan/enum/DynamicState.hpp>
#include <core/vulkan/enum/Format.hpp>
#include <core/vulkan/enum/FrontFace.hpp>
#include <core/vulkan/enum/LogicOp.hpp>
#include <core/vulkan/enum/PolygonMode.hpp>
#include <core/vulkan/enum/PrimitiveTopology.hpp>
#include <core/vulkan/enum/ShaderStage.hpp>
#include <core/vulkan/enum/StencilOp.hpp>
#include <core/vulkan/enum/VertexInputRate.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

CORE_VK_ERROR_WITH_KINDS(GraphicsPipelineCreationError, VulkanRuntimeError,
    NoShadersProvided,
    MissingMeshOrVertexShader,
    ShaderModuleCountMismatch,
    PipelineCreationFailed)

namespace core::vk {

struct RasterizationState final {
    bool depth_clamp_enable = false;
    bool rasterizer_discard_enable = false;
    PolygonMode polygon_mode = PolygonMode::Fill;
    CullMode cull_mode = CullMode::None;
    FrontFace front_face = FrontFace::CounterClockwise;
    bool depth_bias_enable = false;
    float depth_bias_constant_factor = 0.f;
    float depth_bias_clamp = 0.f;
    float depth_bias_slope_factor = 0.f;
    float line_width = 1.f;
};

struct StencilOpState final {
    StencilOp fail_op = StencilOp::Keep;
    StencilOp pass_op = StencilOp::Keep;
    StencilOp depth_fail_op = StencilOp::Keep;
    CompareOp compare_op = CompareOp::Always;
    uint32_t compare_mask = 0xff;
    uint32_t write_mask = 0xff;
    uint32_t reference = 0;
};

struct DepthStencilState final {
    bool depth_test_enable = false;
    bool depth_write_enable = false;
    CompareOp depth_compare_op = CompareOp::Less;
    bool depth_bounds_test_enable = false;
    bool stencil_test_enable = false;
    StencilOpState front;
    StencilOpState back;
    float min_depth_bounds = 0.f;
    float max_depth_bounds = 1.f;
};

struct ColorBlendAttachment final {
    bool blend_enable = false;
    BlendFactor src_color_blend_factor = BlendFactor::One;
    BlendFactor dst_color_blend_factor = BlendFactor::Zero;
    BlendOp color_blend_op = BlendOp::Add;
    BlendFactor src_alpha_blend_factor = BlendFactor::One;
    BlendFactor dst_alpha_blend_factor = BlendFactor::Zero;
    BlendOp alpha_blend_op = BlendOp::Add;
    ColorComponents color_write_mask = ColorComponents::of(
        ColorComponent::R, ColorComponent::G, ColorComponent::B, ColorComponent::A
    );
};

struct ColorBlendState final {
    bool logic_op_enable = false;
    LogicOp logic_op = LogicOp::Copy;
    std::vector<ColorBlendAttachment> attachments;
    float blend_constants[4]{ 0.f, 0.f, 0.f, 0.f };
};

struct InputAssemblyState final {
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    bool primitive_restart_enable = false;
};

struct VertexInputBinding final {
    uint32_t binding = 0;
    uint32_t stride = 0;
    VertexInputRate input_rate = VertexInputRate::Vertex;
};

struct VertexInputAttribute final {
    uint32_t location = 0;
    uint32_t binding = 0;
    Format format;
    uint32_t offset = 0;
};

struct VertexInputState final {
    std::vector<VertexInputBinding> bindings;
    std::vector<VertexInputAttribute> attributes;
};

struct MultisampleState final {
    uint32_t rasterization_samples = 1;
    bool sample_shading_enable = false;
    float min_sample_shading = 1.f;
    std::optional<std::vector<uint32_t>> sample_mask;
    bool alpha_to_coverage_enable = false;
    bool alpha_to_one_enable = false;
};

struct TessellationState final {
    uint32_t patch_control_points = 3;
};

struct DynamicRenderingInfo final {
    std::vector<Format> color_formats;
    std::optional<Format> depth_format;
    std::optional<Format> stencil_format;
};

struct ShaderInfo final {
    ShaderStage stage;
    SpirV const& spirv;
    std::string entry = "main";
};

struct GraphicsPipelineOptions final {
    std::vector<ShaderInfo> shaders;

    std::optional<RasterizationState> rasterization;
    std::optional<DepthStencilState> depth_stencil;
    std::optional<ColorBlendState> color_blend;
    std::optional<InputAssemblyState> input_assembly;
    std::optional<MultisampleState> multisample;
    std::optional<VertexInputState> vertex_input;
    std::optional<TessellationState> tessellation;
    std::vector<DynamicState> dynamic_states;

    std::optional<DynamicRenderingInfo> dynamic_rendering;

    [[nodiscard]]
    GraphicsPipeline build(
        Device const& device,
        PipelineLayout const& layout,
        std::span<RawShaderModule const> const shader_modules,
        VkRenderPass const render_pass = VK_NULL_HANDLE,
        uint32_t const subpass = 0
    ) const;
};

} // namespace core::vk
