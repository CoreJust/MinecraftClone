#include "Pipeline.hpp"
#include <vulkan/vulkan.h>
#include <Core/IO/Logger.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "../Check.hpp"
#include "../Vulkan/Vulkan.hpp"
#include "../Vulkan/SwapchainFormat.hpp"
#include "../Command/CommandBuffer.hpp"
#include "ShaderModule.hpp"
#include "AttributeFormat.hpp"

namespace graphics::vulkan::internal {
namespace {
    VkPipelineViewportStateCreateInfo makeViewportStateCreateInfo(VkExtent2D const& extent, VkViewport& viewport, VkRect2D& scissor) {
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width  = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        scissor.offset = {0, 0};
        scissor.extent = extent;

        VkPipelineViewportStateCreateInfo viewportState { };
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        return viewportState;
    }

    core::ArrayView<VkPushConstantRange> makePushConstantsRanges(
        pipeline::PipelineOptions const& options,
        u32 (&pushConstantsSizes)[2]
    ) {
        VkPushConstantRange pushConstantsRanges[] = {
            makePushConstantsDescription(ShaderStageBit::Vertex,   options.vertexPushContants),
            makePushConstantsDescription(ShaderStageBit::Fragment, options.fragmentPushContants),
        };

        pushConstantsSizes[0] = pushConstantsRanges[0].size;
        pushConstantsSizes[1] = pushConstantsRanges[1].size;
        static VkPushConstantRange nonemptyPushConstantsRanges[sizeof(pushConstantsRanges) / sizeof(pushConstantsRanges[0])];
        VkPushConstantRange* it = nonemptyPushConstantsRanges;
        for (auto const& range : pushConstantsRanges) {
            if (range.size != 0)
                *(it++) = range;
        }
        return { nonemptyPushConstantsRanges, static_cast<usize>(it - nonemptyPushConstantsRanges) };
    }
} // namespace

    Pipeline::Pipeline(Vulkan& vulkan, pipeline::PipelineOptions const& options)
        : m_pass(vulkan)
        , m_framebuffers(vulkan, m_pass)
        , m_vulkan(vulkan) {
        ShaderModule vertex   { m_vulkan, options.vertexShaderPath   ? options.vertexShaderPath   : "basic.vert" };
        ShaderModule fragment { m_vulkan, options.fragmentShaderPath ? options.fragmentShaderPath : "basic.frag" };
        VkPipelineShaderStageCreateInfo shaderStages[] = {
            vertex  .makeCreationInfo(VK_SHADER_STAGE_VERTEX_BIT),
            fragment.makeCreationInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        };

        VulkanVertexLayoutDescription vertexLayoutDescription = makeVertexLayoutDescription(options.attributes);

        VkPipelineVertexInputStateCreateInfo vertexInputInfo { };
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &vertexLayoutDescription.bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<u32>(vertexLayoutDescription.attrDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = vertexLayoutDescription.attrDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly { };
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport { };
        VkRect2D   scissor { };
        VkPipelineViewportStateCreateInfo viewportState = makeViewportStateCreateInfo(m_vulkan.swapchain().format().extent, viewport, scissor);

        VkPipelineRasterizationStateCreateInfo rasterizer { };
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;
        rasterizer.depthBiasConstantFactor = 0.0f;
        rasterizer.depthBiasClamp = 0.0f;
        rasterizer.depthBiasSlopeFactor = 0.0f;

        VkPipelineMultisampleStateCreateInfo multisampling { };
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling.minSampleShading = 1.0f;
        multisampling.pSampleMask = nullptr;
        multisampling.alphaToCoverageEnable = VK_FALSE;
        multisampling.alphaToOneEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment { };
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlending { };
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.logicOp = VK_LOGIC_OP_COPY;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        colorBlending.blendConstants[0] = 0.f;
        colorBlending.blendConstants[1] = 0.f;
        colorBlending.blendConstants[2] = 0.f;
        colorBlending.blendConstants[3] = 0.f;

        core::ArrayView<VkPushConstantRange> pushConstantRanges = makePushConstantsRanges(options, m_pushConstantsSizes);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo { };
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 0;
        pipelineLayoutInfo.pSetLayouts = nullptr;
        pipelineLayoutInfo.pushConstantRangeCount = static_cast<u32>(pushConstantRanges.size());
        pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

        m_layout = m_vulkan.create<vkCreatePipelineLayout>(&pipelineLayoutInfo, nullptr);

        VkGraphicsPipelineCreateInfo pipelineInfo { };
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = nullptr;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = nullptr;
        pipelineInfo.layout = m_layout;
        pipelineInfo.renderPass = m_pass.get();
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineInfo.basePipelineIndex = -1;

        m_pipeline = m_vulkan.create<vkCreateGraphicsPipelines>(VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr);
        core::info("Created Vulkan pipeline");
    }

    Pipeline::~Pipeline() {
        if (m_vulkan.destroy<vkDestroyPipeline>(m_pipeline, nullptr))
            core::info("Destroyed Vulkan pipeline");
        m_vulkan.destroy<vkDestroyPipelineLayout>(m_layout, nullptr);
    }

    void Pipeline::beginRenderPass(CommandBuffer& commandBuffer, u32 index) {
        VkRenderPassBeginInfo renderPassInfo { };
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_pass.get();
        renderPassInfo.framebuffer = m_framebuffers.get()[index];
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = m_vulkan.swapchain().format().extent;

        VkClearValue clearColor = {{{ 0.f, 0.f, 1.f, 1.f }}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;
        vkCmdBeginRenderPass(commandBuffer.get(), &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer.get(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    }

    void Pipeline::endRenderPass(CommandBuffer& commandBuffer) {
        vkCmdEndRenderPass(commandBuffer.get());
    }
} // namespace graphics::vulkan::internal
