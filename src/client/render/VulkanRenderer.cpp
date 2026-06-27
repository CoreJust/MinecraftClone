#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/File.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/Context.hpp>

#include <volk.h>

#include <array>
#include <cstring>

namespace client {

namespace {

constexpr uint32_t kWorldSize = 32;
constexpr uint32_t kGridWorkgroupsX = 32;
constexpr uint32_t kGridWorkgroupsY = 32;
constexpr char const* kGridShaderPath = "build/debug/src/client/grid.mesh.spv";
constexpr char const* kPlayerShaderPath = "build/debug/src/client/player.mesh.spv";
constexpr char const* kFragmentShaderPath = "build/debug/src/client/trivial.frag.spv";

struct alignas(16) GridPushConstants final {
    float worldSize = static_cast<float>(kWorldSize);
    float lineWidth = 0.03f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    std::array<float, 4> lineColor{0.16f, 0.16f, 0.18f, 1.0f};
};
static_assert(sizeof(GridPushConstants) == 32);

struct alignas(16) PlayerPushConstants final {
    std::array<float, 2> origin{0.0f, 0.0f};
    float size = 2.0f;
    float pad0 = 0.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(PlayerPushConstants) == 32);

[[nodiscard]] std::vector<uint32_t> toSpirv(std::vector<uint8_t> const& bytes) {
    ASSERT(bytes.size() % 4 == 0);
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

[[nodiscard]] VkShaderModule loadShaderModule(core::Device const& device, std::string const& path) {
    auto const bytes = core::readFile(path);
    ASSERT(bytes.has_value(), "Failed to read shader file: {}", path);

    auto const words = toSpirv(*bytes);
    VkShaderModuleCreateInfo const info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size() * sizeof(uint32_t),
        .pCode = words.data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    CORE_VK_ASSERT(vkCreateShaderModule(device.handle(), &info, nullptr, &module));
    return module;
}

} // namespace

struct VulkanRenderer::Impl final {
public:
    explicit Impl(core::Window const& window)
        : m_ctx(
            core::VulkanContextBuilder()
                .project(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
                .engine(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
                .requireVersion(core::Version{ 0, 1, 3, 0 })
                .renderTo(window)
                .portabilityEnumeration()
                .requireValidation()
                .requireMeshShaders()
                .requireFeatures({
                    core::VulkanFeature::DynamicRendering,
                    core::VulkanFeature::Synchronization2,
                    core::VulkanFeature::Maintanance4,
                }),
            &window
        )
    {
        CORE_INFO("Loaded Vulkan:\n{}", m_ctx.toString());
        createPipelines();
        m_ctx.onReload([this](
            core::ReloadType const type,
            core::ReloadSource const source,
            core::ReloadAction const action
        ) {
            CORE_WARN("VulkanRenderer received reload of type {} with source {}; action {}", type, source, action);
            if (action == core::ReloadAction::Destroy) {
                destroyPipelines();
            } else {
                createPipelines();
            }
        });
    }

    ~Impl() {
        m_ctx.waitIdle();
        destroyPipelines();
    }

    void render(std::span<PlayerRenderData const> const players) {
        if (m_ctx.window()->isFramebufferSizeZero()) {
            return;
        }
        if (!m_ctx.beginFrame()) {
            CORE_WARN("Failed to wait for in-flight fence on frame {}; skipping the frame", m_ctx.wrappedFrameIndex());
            return;
        }

        core::RawCommandBuffer cmd = m_ctx.commandBuffer();
        VkImageMemoryBarrier2 const toColor{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = m_ctx.swapchainImage().handle(),
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkDependencyInfo const depToColor{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toColor,
        };
        vkCmdPipelineBarrier2(cmd.handle(), &depToColor);

        VkClearValue const clearValue{
            .color = VkClearColorValue{{0.06f, 0.06f, 0.08f, 1.0f}},
        };
        VkRenderingAttachmentInfo const colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = m_ctx.swapchainImageView().handle(),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clearValue,
        };
        VkRenderingInfo const renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {
                .offset = {0, 0},
                .extent = { m_ctx.extent().x, m_ctx.extent().y },
            },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
        };

        vkCmdBeginRendering(cmd.handle(), &renderingInfo);

        VkViewport const viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(m_ctx.extent().x),
            .height = static_cast<float>(m_ctx.extent().y),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D const scissor{
            .offset = {0, 0},
            .extent = { m_ctx.extent().x, m_ctx.extent().y },
        };
        vkCmdSetViewport(cmd.handle(), 0, 1, &viewport);
        vkCmdSetScissor(cmd.handle(), 0, 1, &scissor);

        vkCmdBindPipeline(cmd.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        GridPushConstants const gridPush{};
        vkCmdPushConstants(cmd.handle(), m_gridLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(GridPushConstants), &gridPush);
        vkCmdDrawMeshTasksEXT(cmd.handle(), kGridWorkgroupsX, kGridWorkgroupsY, 1);

        vkCmdBindPipeline(cmd.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_playerPipeline);
        for (auto const& player : players) {
            PlayerPushConstants push{};
            push.origin = {static_cast<float>(player.x), static_cast<float>(player.y)};
            push.size = 2.0f;
            push.color = player.color;
            vkCmdPushConstants(cmd.handle(), m_playerLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(PlayerPushConstants), &push);
            vkCmdDrawMeshTasksEXT(cmd.handle(), 1, 1, 1);
        }

        vkCmdEndRendering(cmd.handle());

        VkImageMemoryBarrier2 const toPresent{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = VK_ACCESS_2_NONE,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .image = m_ctx.swapchainImage().handle(),
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkDependencyInfo const depToPresent{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toPresent,
        };
        vkCmdPipelineBarrier2(cmd.handle(), &depToPresent);

        m_ctx.endFrame();
    }

    void hotReload() {
        m_ctx.reload(core::ReloadType::Instance);
    }
private:
    void createPipelines() {
        m_gridMeshShader = loadShaderModule(m_ctx.device(), kGridShaderPath);
        m_playerMeshShader = loadShaderModule(m_ctx.device(), kPlayerShaderPath);
        m_fragmentShader = loadShaderModule(m_ctx.device(), kFragmentShaderPath);

        VkPushConstantRange const gridRange{
            .stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT,
            .offset = 0,
            .size = sizeof(GridPushConstants),
        };
        VkPushConstantRange const playerRange{
            .stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT,
            .offset = 0,
            .size = sizeof(PlayerPushConstants),
        };

        VkPipelineLayoutCreateInfo const gridLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &gridRange,
        };
        VkPipelineLayoutCreateInfo const playerLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &playerRange,
        };

        CORE_VK_ASSERT(vkCreatePipelineLayout(m_ctx.device().handle(), &gridLayoutInfo, nullptr, &m_gridLayout));
        CORE_VK_ASSERT(vkCreatePipelineLayout(m_ctx.device().handle(), &playerLayoutInfo, nullptr, &m_playerLayout));

        VkFormat const format = static_cast<VkFormat>(m_ctx.surfaceFormat());
        VkPipelineRenderingCreateInfo const gridRendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &format,
        };
        VkPipelineRenderingCreateInfo const playerRendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &format,
        };

        VkPipelineShaderStageCreateInfo const gridStages[2]{
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
                .module = m_gridMeshShader,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = m_fragmentShader,
                .pName = "main",
            },
        };
        VkPipelineShaderStageCreateInfo const playerStages[2]{
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
                .module = m_playerMeshShader,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = m_fragmentShader,
                .pName = "main",
            },
        };

        VkPipelineVertexInputStateCreateInfo const vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo const inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };
        VkPipelineViewportStateCreateInfo const viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo const rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo const multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState const attachment{
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                | VK_COLOR_COMPONENT_G_BIT
                | VK_COLOR_COMPONENT_B_BIT
                | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo const blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &attachment,
        };
        VkDynamicState const dynamicStates[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo const dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates,
        };

        VkGraphicsPipelineCreateInfo const gridInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &gridRendering,
            .stageCount = 2,
            .pStages = gridStages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamicState,
            .layout = m_gridLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
        };
        VkGraphicsPipelineCreateInfo const playerInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &playerRendering,
            .stageCount = 2,
            .pStages = playerStages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamicState,
            .layout = m_playerLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
        };

        VkGraphicsPipelineCreateInfo const infos[2]{gridInfo, playerInfo};
        VkPipeline pipelines[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        CORE_VK_ASSERT(vkCreateGraphicsPipelines(m_ctx.device().handle(), VK_NULL_HANDLE, 2, infos, nullptr, pipelines));

        m_gridPipeline = pipelines[0];
        m_playerPipeline = pipelines[1];
    }

    void destroyPipelines() {
        if (!m_ctx.device().isNull()) {
            if (m_gridPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_ctx.device().handle(), m_gridPipeline, nullptr);
                m_gridPipeline = VK_NULL_HANDLE;
            }
            if (m_playerPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_ctx.device().handle(), m_playerPipeline, nullptr);
                m_playerPipeline = VK_NULL_HANDLE;
            }
            if (m_gridLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_ctx.device().handle(), m_gridLayout, nullptr);
                m_gridLayout = VK_NULL_HANDLE;
            }
            if (m_playerLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_ctx.device().handle(), m_playerLayout, nullptr);
                m_playerLayout = VK_NULL_HANDLE;
            }
            if (m_gridMeshShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_ctx.device().handle(), m_gridMeshShader, nullptr);
                m_gridMeshShader = VK_NULL_HANDLE;
            }
            if (m_playerMeshShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_ctx.device().handle(), m_playerMeshShader, nullptr);
                m_playerMeshShader = VK_NULL_HANDLE;
            }
            if (m_fragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_ctx.device().handle(), m_fragmentShader, nullptr);
                m_fragmentShader = VK_NULL_HANDLE;
            }
        }
    }
private:
    core::VulkanContext m_ctx;

    VkShaderModule m_gridMeshShader = VK_NULL_HANDLE;
    VkShaderModule m_playerMeshShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShader = VK_NULL_HANDLE;
    VkPipelineLayout m_gridLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_playerLayout = VK_NULL_HANDLE;
    VkPipeline m_gridPipeline = VK_NULL_HANDLE;
    VkPipeline m_playerPipeline = VK_NULL_HANDLE;
};

VulkanRenderer::VulkanRenderer(core::Window const& window)
    : m_impl(std::make_unique<Impl>(window))
{ }

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::render(std::span<PlayerRenderData const> const players) {
    m_impl->render(players);
}

void VulkanRenderer::hotReload() {
    m_impl->hotReload();
}

} // namespace client
