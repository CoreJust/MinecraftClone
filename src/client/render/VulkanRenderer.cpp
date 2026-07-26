#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/File.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/FrameGraph.hpp>
#include <core/vulkan/GraphicsPipelineOptions.hpp>

#include <array>

#define SPIR_V_PATH(name) "build/debug/src/client/" name ".spv"

namespace client {

namespace vk = core::vk;

namespace {

constexpr uint32_t kGridWorkgroupsX = 32;
constexpr uint32_t kGridWorkgroupsY = 32;

struct alignas(16) GridPushConstants final {
    float worldSize = 32.f;
    float lineWidth = 0.03f;
    float pad0 = 0.f;
    float pad1 = 0.f;
    std::array<float, 4> lineColor{ 0.16f, 0.16f, 0.18f, 1.f };
};
static_assert(sizeof(GridPushConstants) == 32);

struct alignas(16) PlayerPushConstants final {
    std::array<float, 2> origin{ 0.f, 0.f };
    float size = 2.f;
    float pad0 = 0.f;
    std::array<float, 4> color{ 1.f, 1.f, 1.f, 1.f};
};
static_assert(sizeof(PlayerPushConstants) == 32);

} // namespace

struct VulkanRenderer::Impl final {
public:
    explicit Impl(core::Window const& window)
        : m_graph(vk::VulkanContext(
            vk::VulkanContextBuilder()
                .project(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
                .engine(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
                .requireVersion(core::Version{ 0, 1, 3, 0 })
                .renderTo(window)
                .portabilityEnumeration()
                .requireValidation()
                .requireMeshShaders()
                .requireFeatures({
                    vk::VulkanFeature::DynamicRendering,
                    vk::VulkanFeature::Synchronization2,
                    vk::VulkanFeature::Maintanance4,
                }),
            &window
        ))
    {
        CORE_INFO("Loaded Vulkan:\n{}", m_graph.ctx().toString());
        createPipelines();

        auto swapchain = m_graph.importSwapchain(core::Color4{ 0.06f, 0.06f, 0.08f, 1.0f });
        m_render_pass = m_graph.add(vk::FramePassOptions{
            .name = "render_pass",
            .written_resources = { swapchain },
        });

        m_graph.onReload([this](
            vk::ReloadType const type,
            vk::ReloadSource const source,
            vk::ReloadAction const action
        ) {
            CORE_WARN("VulkanRenderer received reload of type {} with source {}; action {}", type, source, action);
            if (action == vk::ReloadAction::Destroy) {
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
        GridPushConstants const grid_push{ };
        m_graph.bind(m_render_pass, [&](vk::FramePassContext const ctx) {
            m_grid_pipeline.execute(ctx.cmd, [&](vk::BoundGraphicsPipeline p) {
                p.pushConstants(vk::ShaderStage::Mesh, 0, grid_push);
                p.drawMeshTasks(kGridWorkgroupsX, kGridWorkgroupsY);
            });
            m_player_pipeline.execute(ctx.cmd, [&](vk::BoundGraphicsPipeline p) {
                for (PlayerRenderData const& player : players) {
                    PlayerPushConstants push{
                        .origin = {
                            static_cast<float>(player.x),
                            static_cast<float>(player.y),
                        },
                        .size = 2.0f,
                        .color = player.color,
                    };
                    p.pushConstants(vk::ShaderStage::Mesh, 0, push);
                    p.drawMeshTasks();
                }
            });
        });
        m_graph.render();
    }

    void hotReload() {
        m_graph.reload(vk::ReloadType::Instance);
    }
private:
    void createPipelines() {
        vk::Device& dev = m_graph.ctx().device();
        vk::SpirV grid_mesh = vk::SpirV::fromFile(SPIR_V_PATH("grid.mesh"));
        vk::SpirV player_mesh = vk::SpirV::fromFile(SPIR_V_PATH("player.mesh"));
        vk::SpirV trivial_frag = vk::SpirV::fromFile(SPIR_V_PATH("trivial.frag"));
        m_grid_shader = vk::ShaderModule{ dev, grid_mesh };
        m_player_shader = vk::ShaderModule{ dev, player_mesh };
        m_fragment_shader = vk::ShaderModule{ dev, trivial_frag };

        m_grid_layout = vk::PipelineLayout{
            dev,
            vk::PipelineLayout::Info::fromSpirVs(grid_mesh, trivial_frag),
        };
        m_player_layout = vk::PipelineLayout{
            dev,
            vk::PipelineLayout::Info::fromSpirVs(player_mesh, trivial_frag),
        };

        vk::GraphicsPipelineOptions grid_opts{
            .shaders = {
                {vk::ShaderStage::Mesh,     grid_mesh,    "main"},
                {vk::ShaderStage::Fragment, trivial_frag, "main"}
            },
            .dynamic_rendering = vk::DynamicRenderingInfo{
                .color_formats = {m_graph.ctx().surfaceFormat()}
            }
        };

        m_grid_pipeline = grid_opts.build(
            dev,
            m_grid_layout,
            std::array{m_grid_shader.raw(), m_fragment_shader.raw()}
        );

        vk::GraphicsPipelineOptions player_opts{
            .shaders = {
                {vk::ShaderStage::Mesh,     player_mesh,   "main"},
                {vk::ShaderStage::Fragment, trivial_frag,  "main"}
            },
            .dynamic_rendering = vk::DynamicRenderingInfo{
                .color_formats = {m_graph.ctx().surfaceFormat()}
            }
        };

        m_player_pipeline = player_opts.build(
            dev,
            m_player_layout,
            std::array{m_player_shader.raw(), m_fragment_shader.raw()}
        );
    }

    void destroyPipelines() {
        vk::Device& device = m_graph.ctx().device();
        if (!device.isNull()) {
            m_grid_pipeline = {};
            m_player_pipeline = {};
            m_grid_layout = {};
            m_player_layout = {};
            m_grid_shader = {};
            m_player_shader = {};
            m_fragment_shader = {};
        }
    }
private:
    vk::FrameGraph m_graph;
    vk::FramePassId m_render_pass;

    vk::ShaderModule m_grid_shader;
    vk::ShaderModule m_player_shader;
    vk::ShaderModule m_fragment_shader;
    vk::PipelineLayout m_grid_layout;
    vk::PipelineLayout m_player_layout;
    vk::GraphicsPipeline m_grid_pipeline;
    vk::GraphicsPipeline m_player_pipeline;
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
