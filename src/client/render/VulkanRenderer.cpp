#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/File.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/FrameGraph.hpp>

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

[[nodiscard]] VkShaderModule loadShaderModule(core::vk::Device const& device, std::string const& path) {
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
        : m_graph(core::vk::VulkanContext(
            core::vk::VulkanContextBuilder()
                .project(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
                .engine(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
                .requireVersion(core::Version{ 0, 1, 3, 0 })
                .renderTo(window)
                .portabilityEnumeration()
                .requireValidation()
                .requireMeshShaders()
                .requireFeatures({
                    core::vk::VulkanFeature::DynamicRendering,
                    core::vk::VulkanFeature::Synchronization2,
                    core::vk::VulkanFeature::Maintanance4,
                }),
            &window
        ))
    {
        CORE_INFO("Loaded Vulkan:\n{}", m_graph.ctx().toString());
        createPipelines();

        auto swapchain = m_graph.importSwapchain(core::Color4{ 0.06f, 0.06f, 0.08f, 1.0f });
        m_render_pass = m_graph.add(core::vk::FramePassOptions{
            .name = "render_pass",
            .written_resources = { swapchain },
        });

        m_graph.onReload([this](
            core::vk::ReloadType const type,
            core::vk::ReloadSource const source,
            core::vk::ReloadAction const action
        ) {
            CORE_WARN("VulkanRenderer received reload of type {} with source {}; action {}", type, source, action);
            if (action == core::vk::ReloadAction::Destroy) {
                destroyPipelines();
            } else {
                createPipelines();
            }
        });
    }

    ~Impl() {
        m_graph.ctx().waitIdle();
        destroyPipelines();
    }

    void render(std::span<PlayerRenderData const> const players) {
        m_graph.bind(m_render_pass, [&](core::vk::FramePassContext const ctx) {
            VkCommandBuffer const cmd = ctx.cmd.handle();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);

            GridPushConstants const grid_push{ };
            vkCmdPushConstants(
                cmd,
                m_gridLayout,
                VK_SHADER_STAGE_MESH_BIT_EXT,
                0,
                sizeof(GridPushConstants),
                &grid_push
            );
            vkCmdDrawMeshTasksEXT(cmd, kGridWorkgroupsX, kGridWorkgroupsY, 1);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_playerPipeline);
            for (PlayerRenderData const& player : players) {
                PlayerPushConstants push{
                    .origin = {
                        static_cast<float>(player.x),
                        static_cast<float>(player.y),
                    },
                    .size = 2.0f,
                    .color = player.color,
                };
                vkCmdPushConstants(
                    cmd,
                    m_playerLayout,
                    VK_SHADER_STAGE_MESH_BIT_EXT,
                    0,
                    sizeof(PlayerPushConstants),
                    &push
                );
                vkCmdDrawMeshTasksEXT(cmd, 1, 1, 1);
            }
        });
        m_graph.render();
    }

    void hotReload() {
        m_graph.reload(core::vk::ReloadType::Instance);
    }
private:
    void createPipelines() {
        m_gridMeshShader = loadShaderModule(m_graph.ctx().device(), kGridShaderPath);
        m_playerMeshShader = loadShaderModule(m_graph.ctx().device(), kPlayerShaderPath);
        m_fragmentShader = loadShaderModule(m_graph.ctx().device(), kFragmentShaderPath);

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

        CORE_VK_ASSERT(vkCreatePipelineLayout(m_graph.ctx().device().handle(), &gridLayoutInfo, nullptr, &m_gridLayout));
        CORE_VK_ASSERT(vkCreatePipelineLayout(m_graph.ctx().device().handle(), &playerLayoutInfo, nullptr, &m_playerLayout));

        VkFormat const format = static_cast<VkFormat>(m_graph.ctx().surfaceFormat());
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
        CORE_VK_ASSERT(vkCreateGraphicsPipelines(m_graph.ctx().device().handle(), VK_NULL_HANDLE, 2, infos, nullptr, pipelines));

        m_gridPipeline = pipelines[0];
        m_playerPipeline = pipelines[1];
    }

    void destroyPipelines() {
        core::vk::Device& device = m_graph.ctx().device();
        if (!device.isNull()) {
            if (m_gridPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device.handle(), m_gridPipeline, nullptr);
                m_gridPipeline = VK_NULL_HANDLE;
            }
            if (m_playerPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device.handle(), m_playerPipeline, nullptr);
                m_playerPipeline = VK_NULL_HANDLE;
            }
            if (m_gridLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device.handle(), m_gridLayout, nullptr);
                m_gridLayout = VK_NULL_HANDLE;
            }
            if (m_playerLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device.handle(), m_playerLayout, nullptr);
                m_playerLayout = VK_NULL_HANDLE;
            }
            if (m_gridMeshShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device.handle(), m_gridMeshShader, nullptr);
                m_gridMeshShader = VK_NULL_HANDLE;
            }
            if (m_playerMeshShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device.handle(), m_playerMeshShader, nullptr);
                m_playerMeshShader = VK_NULL_HANDLE;
            }
            if (m_fragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device.handle(), m_fragmentShader, nullptr);
                m_fragmentShader = VK_NULL_HANDLE;
            }
        }
    }
private:
    core::vk::FrameGraph m_graph;
    core::vk::FramePassId m_render_pass;

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
