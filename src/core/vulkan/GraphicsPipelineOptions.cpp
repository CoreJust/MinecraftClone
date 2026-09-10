#include <core/vulkan/GraphicsPipelineOptions.hpp>

#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>

#include <volk.h>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::GraphicsPipelineCreationErrorKind);

namespace core::vk {

namespace {

[[nodiscard]]
VkPipelineShaderStageCreateInfo toVkShaderStageCreateInfo(
    ShaderInfo const& shader,
    VkShaderModule const module
) {
    return VkPipelineShaderStageCreateInfo{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = static_cast<VkShaderStageFlagBits>(ShaderStages{ shader.stage }.value),
        .module = module,
        .pName  = shader.entry.c_str(),
    };
}

[[nodiscard]]
VkPipelineRasterizationStateCreateInfo toVkRasterizationState(RasterizationState const& state) {
    return VkPipelineRasterizationStateCreateInfo{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable        = state.depth_clamp_enable ? VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = state.rasterizer_discard_enable ? VK_TRUE : VK_FALSE,
        .polygonMode             = toVk<VkPolygonMode>(state.polygon_mode),
        .cullMode                = toVk<VkCullModeFlags>(state.cull_mode),
        .frontFace               = toVk<VkFrontFace>(state.front_face),
        .depthBiasEnable         = state.depth_bias_enable ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = state.depth_bias_constant_factor,
        .depthBiasClamp          = state.depth_bias_clamp,
        .depthBiasSlopeFactor    = state.depth_bias_slope_factor,
        .lineWidth               = state.line_width,
    };
}

[[nodiscard]]
VkStencilOpState toVkStencilOpState(StencilOpState const& state) {
    return VkStencilOpState{
        .failOp      = toVk<VkStencilOp>(state.fail_op),
        .passOp      = toVk<VkStencilOp>(state.pass_op),
        .depthFailOp = toVk<VkStencilOp>(state.depth_fail_op),
        .compareOp   = toVk<VkCompareOp>(state.compare_op),
        .compareMask = state.compare_mask,
        .writeMask   = state.write_mask,
        .reference   = state.reference,
    };
}

[[nodiscard]]
VkPipelineDepthStencilStateCreateInfo toVkDepthStencilState(DepthStencilState const& state) {
    return VkPipelineDepthStencilStateCreateInfo{
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable       = state.depth_test_enable ? VK_TRUE : VK_FALSE,
        .depthWriteEnable      = state.depth_write_enable ? VK_TRUE : VK_FALSE,
        .depthCompareOp        = toVk<VkCompareOp>(state.depth_compare_op),
        .depthBoundsTestEnable = state.depth_bounds_test_enable ? VK_TRUE : VK_FALSE,
        .stencilTestEnable     = state.stencil_test_enable ? VK_TRUE : VK_FALSE,
        .front                 = toVkStencilOpState(state.front),
        .back                  = toVkStencilOpState(state.back),
        .minDepthBounds        = state.min_depth_bounds,
        .maxDepthBounds        = state.max_depth_bounds,
    };
}

[[nodiscard]]
VkPipelineColorBlendAttachmentState toVkColorBlendAttachment(ColorBlendAttachment const& attachment) {
    return VkPipelineColorBlendAttachmentState{
        .blendEnable         = attachment.blend_enable ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = toVk<VkBlendFactor>(attachment.src_color_blend_factor),
        .dstColorBlendFactor = toVk<VkBlendFactor>(attachment.dst_color_blend_factor),
        .colorBlendOp        = toVk<VkBlendOp>(attachment.color_blend_op),
        .srcAlphaBlendFactor = toVk<VkBlendFactor>(attachment.src_alpha_blend_factor),
        .dstAlphaBlendFactor = toVk<VkBlendFactor>(attachment.dst_alpha_blend_factor),
        .alphaBlendOp        = toVk<VkBlendOp>(attachment.alpha_blend_op),
        .colorWriteMask      = static_cast<VkColorComponentFlags>(attachment.color_write_mask.value),
    };
}

[[nodiscard]]
VkPipelineColorBlendStateCreateInfo toVkColorBlendState(ColorBlendState const& state) {
    static thread_local std::vector<VkPipelineColorBlendAttachmentState> attachments;
    attachments.clear();
    for (ColorBlendAttachment const& attachment : state.attachments) {
        attachments.push_back(toVkColorBlendAttachment(attachment));
    }
    return VkPipelineColorBlendStateCreateInfo{
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable   = state.logic_op_enable ? VK_TRUE : VK_FALSE,
        .logicOp         = toVk<VkLogicOp>(state.logic_op),
        .attachmentCount = static_cast<uint32_t>(attachments.size()),
        .pAttachments    = attachments.data(),
        .blendConstants  = {
            state.blend_constants[0],
            state.blend_constants[1],
            state.blend_constants[2],
            state.blend_constants[3]
        },
    };
}

[[nodiscard]]
VkPipelineInputAssemblyStateCreateInfo toVkInputAssemblyState(InputAssemblyState const& state) {
    return VkPipelineInputAssemblyStateCreateInfo{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = toVk<VkPrimitiveTopology>(state.topology),
        .primitiveRestartEnable = state.primitive_restart_enable ? VK_TRUE : VK_FALSE,
    };
}

[[nodiscard]]
VkPipelineVertexInputStateCreateInfo toVkVertexInputState(VertexInputState const& state) {
    static thread_local std::vector<VkVertexInputBindingDescription> bindings;
    static thread_local std::vector<VkVertexInputAttributeDescription> attributes;
    bindings.clear();
    attributes.clear();
    for (VertexInputBinding const& binding : state.bindings) {
        bindings.push_back(VkVertexInputBindingDescription{
            .binding   = binding.binding,
            .stride    = binding.stride,
            .inputRate = toVk<VkVertexInputRate>(binding.input_rate),
        });
    }
    for (VertexInputAttribute const& attribute : state.attributes) {
        attributes.push_back(VkVertexInputAttributeDescription{
            .location = attribute.location,
            .binding  = attribute.binding,
            .format   = toVk<VkFormat>(attribute.format),
            .offset   = attribute.offset,
        });
    }
    return VkPipelineVertexInputStateCreateInfo{
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = static_cast<uint32_t>(bindings.size()),
        .pVertexBindingDescriptions      = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions    = attributes.data(),
    };
}

[[nodiscard]]
VkPipelineMultisampleStateCreateInfo toVkMultisampleState(MultisampleState const& state) {
    return VkPipelineMultisampleStateCreateInfo{
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples  = static_cast<VkSampleCountFlagBits>(state.rasterization_samples),
        .sampleShadingEnable   = state.sample_shading_enable ? VK_TRUE : VK_FALSE,
        .minSampleShading      = state.min_sample_shading,
        .pSampleMask           = state.sample_mask.has_value() ? state.sample_mask->data() : nullptr,
        .alphaToCoverageEnable = state.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE,
        .alphaToOneEnable      = state.alpha_to_one_enable ? VK_TRUE : VK_FALSE,
    };
}

[[nodiscard]]
VkPipelineTessellationStateCreateInfo toVkTessellationState(TessellationState const& state) {
    return VkPipelineTessellationStateCreateInfo{
        .sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = state.patch_control_points,
    };
}

[[nodiscard]]
VkPipelineDynamicStateCreateInfo toVkDynamicState(std::vector<DynamicState> const& states) {
    static thread_local std::vector<VkDynamicState> vk_states;
    vk_states.clear();
    for (DynamicState const dynamic_state : states) {
        vk_states.push_back(toVk<VkDynamicState>(dynamic_state));
    }
    if (vk_states.empty()) {
        vk_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    }
    return VkPipelineDynamicStateCreateInfo{
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(vk_states.size()),
        .pDynamicStates    = vk_states.data(),
    };
}

[[nodiscard]]
VkPipelineViewportStateCreateInfo emptyViewportState() {
    return VkPipelineViewportStateCreateInfo{
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
}

[[nodiscard]]
VkPipelineRenderingCreateInfo toVkDynamicRenderingInfo(DynamicRenderingInfo const& info) {
    static thread_local std::vector<VkFormat> formats;
    formats.clear();
    for (Format const format : info.color_formats) {
        formats.push_back(toVk<VkFormat>(format));
    }
    return VkPipelineRenderingCreateInfo{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = static_cast<uint32_t>(formats.size()),
        .pColorAttachmentFormats = formats.data(),
        .depthAttachmentFormat   = info.depth_format
            ? toVk<VkFormat>(*info.depth_format)
            : VK_FORMAT_UNDEFINED
        ,
        .stencilAttachmentFormat = info.stencil_format
            ? toVk<VkFormat>(*info.stencil_format)
            : VK_FORMAT_UNDEFINED
        ,
    };
}

void validateOptions(GraphicsPipelineOptions const& opts, size_t const shader_module_count) {
    if (opts.shaders.empty()) {
        throw GraphicsPipelineCreationError(GraphicsPipelineCreationError::NoShadersProvided);
    }
    bool has_mesh = false;
    bool has_vertex = false;
    for (ShaderInfo const& shader : opts.shaders) {
        if (shader.stage == ShaderStage::Mesh) {
            has_mesh = true;
        }
        if (shader.stage == ShaderStage::Vertex) {
            has_vertex = true;
        }
    }
    if (!has_mesh && !has_vertex) {
        throw GraphicsPipelineCreationError(GraphicsPipelineCreationError::MissingMeshOrVertexShader);
    }
    if (opts.shaders.size() != shader_module_count) {
        throw GraphicsPipelineCreationError(GraphicsPipelineCreationError::ShaderModuleCountMismatch);
    }
}

[[nodiscard]]
ColorBlendState ensureColorBlendAttachments(
    ColorBlendState&& state,
    bool const dynamic,
    uint32_t const attachment_count
) {
    if (state.attachments.empty()) {
        uint32_t const count = dynamic ? attachment_count : 1u;
        state.attachments.resize(count, ColorBlendAttachment{});
    }
    return state;
}

} // namespace

GraphicsPipeline GraphicsPipelineOptions::build(
    Device const& device,
    PipelineLayout const& layout,
    std::span<RawShaderModule const> const shader_modules,
    VkRenderPass const render_pass,
    uint32_t const subpass
) const {
    validateOptions(*this, shader_modules.size());

    bool const use_dynamic_rendering = dynamic_rendering.has_value();

    std::vector<VkPipelineShaderStageCreateInfo> stage_infos;
    stage_infos.reserve(shaders.size());
    for (size_t i = 0; i < shaders.size(); ++i) {
        stage_infos.push_back(toVkShaderStageCreateInfo(shaders[i], shader_modules[i].handle()));
    }

    RasterizationState const raster = rasterization.value_or(RasterizationState{});
    DepthStencilState const depth_stencil_state = depth_stencil.value_or(DepthStencilState{});
    ColorBlendState color_blend_state = color_blend.value_or(ColorBlendState{});
    InputAssemblyState const input_assembly_state = input_assembly.value_or(InputAssemblyState{});
    MultisampleState const multisample_state = multisample.value_or(MultisampleState{});
    VertexInputState const vertex_input_state = vertex_input.value_or(VertexInputState{});
    TessellationState const tessellation_state = tessellation.value_or(TessellationState{});
    std::vector<DynamicState> const dynamic_states_state = dynamic_states.empty()
        ? std::vector<DynamicState>{DynamicState::Viewport, DynamicState::Scissor}
        : dynamic_states
    ;

    color_blend_state = ensureColorBlendAttachments(
        std::move(color_blend_state),
        use_dynamic_rendering,
        use_dynamic_rendering ? static_cast<uint32_t>(dynamic_rendering->color_formats.size()) : 1u
    );

    VkPipelineRasterizationStateCreateInfo const vk_raster = toVkRasterizationState(raster);
    VkPipelineDepthStencilStateCreateInfo const vk_depth_stencil = toVkDepthStencilState(depth_stencil_state);
    VkPipelineColorBlendStateCreateInfo const vk_color_blend = toVkColorBlendState(color_blend_state);
    VkPipelineInputAssemblyStateCreateInfo const vk_input_assembly = toVkInputAssemblyState(input_assembly_state);
    VkPipelineVertexInputStateCreateInfo const vk_vertex_input = toVkVertexInputState(vertex_input_state);
    VkPipelineMultisampleStateCreateInfo const vk_multisample = toVkMultisampleState(multisample_state);
    VkPipelineTessellationStateCreateInfo const vk_tess = toVkTessellationState(tessellation_state);
    VkPipelineDynamicStateCreateInfo const vk_dynamic = toVkDynamicState(dynamic_states_state);
    VkPipelineViewportStateCreateInfo const vk_viewport = emptyViewportState();

    VkPipelineRenderingCreateInfo vk_dynamic_rendering{ };
    if (use_dynamic_rendering) {
        vk_dynamic_rendering = toVkDynamicRenderingInfo(*dynamic_rendering);
    }

    VkGraphicsPipelineCreateInfo pipeline_create_info{
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = use_dynamic_rendering ? &vk_dynamic_rendering : nullptr,
        .stageCount          = static_cast<uint32_t>(stage_infos.size()),
        .pStages             = stage_infos.data(),
        .pVertexInputState   = &vk_vertex_input,
        .pInputAssemblyState = &vk_input_assembly,
        .pTessellationState  = tessellation.has_value() ? &vk_tess : nullptr,
        .pViewportState      = &vk_viewport,
        .pRasterizationState = &vk_raster,
        .pMultisampleState   = &vk_multisample,
        .pDepthStencilState  = &vk_depth_stencil,
        .pColorBlendState    = &vk_color_blend,
        .pDynamicState       = &vk_dynamic,
        .layout              = layout.handle(),
        .renderPass          = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass,
        .subpass             = subpass,
        .basePipelineHandle  = VK_NULL_HANDLE,
        .basePipelineIndex   = -1,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateGraphicsPipelines(
            device.handle(),
            VK_NULL_HANDLE,
            1,
            &pipeline_create_info,
            nullptr,
            &pipeline
    ))) {
        throw GraphicsPipelineCreationError(GraphicsPipelineCreationError::PipelineCreationFailed);
    }

    return GraphicsPipeline(device, pipeline, layout.raw());
}

} // namespace core::vk
