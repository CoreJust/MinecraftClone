#include <client/render/VulkanRenderer.hpp>

#include "MeshShaderSelection.hpp"

#include <shared/ProjectInfo.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/Log.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/FrameContext.hpp>
#include <core/vulkan/FrameGraph.hpp>
#include <core/vulkan/GraphicsPipelineOptions.hpp>
#include <core/vulkan/ImageMemoryBarrier.hpp>

#include <volk.h>

#include <array>
#include <chrono>
#include <limits>
#include <utility>

namespace client {

namespace vk = core::vk;

namespace {

#if (!defined(__ANDROID__) && defined(_CORE_DEBUG)) || defined(_MC_VK_VALIDATION_LAYERS)
constexpr bool REQUIRE_VALIDATION = true;
#else
constexpr bool REQUIRE_VALIDATION = false;
#endif

constexpr uint32_t kGridWorkgroupsX = 32;
constexpr uint32_t kGridWorkgroupsY = 32;
constexpr uint32_t kQuadVertexCount = 6;

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

[[nodiscard]] vk::VulkanContextBuilder makeContextBuilder(
    core::vk::SurfaceProvider const& surface_provider,
    VulkanRendererOptions const options
) {
    auto builder = vk::VulkanContextBuilder()
        .project(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
        .engine(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
        .requireVersion(core::Version{ 0, 1, 2, 0 })
        .renderTo(surface_provider)
        .portabilityEnumeration()
        .requireValidation(REQUIRE_VALIDATION || options.require_validation)
        .preferMeshShaders()
        .requireExtensions({
            vk::VulkanExtension::DynamicRendering,
            vk::VulkanExtension::Synchronization2,
        })
        .requireFeatures({
            vk::VulkanFeature::DynamicRendering,
            vk::VulkanFeature::Synchronization2,
        });
    if (options.enable_frame_capture) {
        (void)builder.preferSwapchainImageUsage(vk::ImageUsage::TransferSrc);
    }
    if (options.require_immediate_present_mode) {
        (void)builder.requirePresentModes({ vk::PresentMode::Immediate });
    }
    return builder;
}

[[nodiscard]] bool isSupportedCaptureFormat(vk::Format const format) noexcept {
    return format == vk::Format::B8G8R8A8SRGB
        || format == vk::Format::B8G8R8A8UNorm
        || format == vk::Format::R8G8B8A8SRGB
        || format == vk::Format::R8G8B8A8UNorm;
}

[[nodiscard]] bool isSrgbCaptureFormat(vk::Format const format) noexcept {
    return format == vk::Format::B8G8R8A8SRGB || format == vk::Format::R8G8B8A8SRGB;
}

[[nodiscard]] RendererPresentMode rendererPresentMode(vk::PresentMode const mode) noexcept {
    switch (mode) {
        case vk::PresentMode::Immediate: return RendererPresentMode::Immediate;
        case vk::PresentMode::Mailbox: return RendererPresentMode::Mailbox;
        case vk::PresentMode::FIFO: return RendererPresentMode::FIFO;
        case vk::PresentMode::FIFORelaxed: return RendererPresentMode::FIFORelaxed;
        default: return RendererPresentMode::Unknown;
    }
}

} // namespace

struct VulkanRenderer::Impl final {
public:
    explicit Impl(
        vk::SurfaceProvider const& surface_provider,
        ShaderAssets const& shader_assets,
        VulkanRendererOptions const options
    )
        : m_shader_assets(shader_assets)
        , m_options(options)
        , m_graph(vk::VulkanContext(
            makeContextBuilder(surface_provider, options),
            &surface_provider
        ))
    {
        CORE_INFO("Loaded Vulkan:\n{}", m_graph.ctx().toString());
        createPipelines();
        refreshCaptureState();

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
                destroyCaptureBuffer();
                destroyPipelines();
            } else {
                createPipelines();
                refreshCaptureState();
            }
        });
        m_graph.beforeFrameSubmit([this](vk::FrameContext& frame) {
            recordCapture(frame);
        });
    }

    ~Impl() {
        m_graph.ctx().waitIdle();
        destroyCaptureBuffer();
        destroyPipelines();
    }

    [[nodiscard]]
    bool render(
        std::span<PlayerRenderData const> const players,
        std::chrono::steady_clock::time_point const deadline
    ) {
        if (m_capture_requested && m_capture_state == FrameCaptureState::Ready
            && !prepareCaptureBuffer()
        ) {
            m_capture_requested = false;
            m_capture_state = FrameCaptureState::Failed;
        }

        GridPushConstants const grid_push{ };
        m_graph.bind(m_render_pass, [&](vk::FramePassContext const ctx) {
            m_grid_pipeline.execute(ctx.cmd, [&](vk::BoundGraphicsPipeline p) {
                p.pushConstants(m_main_shader_stage, 0, grid_push);
                if (m_mesh_shaders) {
                    p.drawMeshTasks(kGridWorkgroupsX, kGridWorkgroupsY);
                } else {
                    p.draw(kQuadVertexCount, kGridWorkgroupsX * kGridWorkgroupsY);
                }
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
                    p.pushConstants(m_main_shader_stage, 0, push);
                    if (m_mesh_shaders) {
                        p.drawMeshTasks();
                    } else {
                        p.draw(kQuadVertexCount);
                    }
                }
            });
        });
        std::chrono::steady_clock::time_point const started_at = std::chrono::steady_clock::now();
        bool const rendered = m_graph.render(deadline);
        m_cpu_frame_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at
        );
        if (rendered) {
            ++m_submitted_frame_count;
        }
        if (m_capture_pending_readback) {
            if (!m_graph.ctx().waitForSubmittedFrames(deadline)) {
                m_capture_pending_readback = false;
                m_capture_state = FrameCaptureState::Failed;
                return false;
            }
            readCaptureBuffer();
        }
        return rendered;
    }

    void hotReload() {
        m_graph.reload(vk::ReloadType::Instance);
    }

    void waitIdle() {
        m_graph.ctx().waitIdle();
    }

    [[nodiscard]] bool waitForSubmittedFrames(
        std::chrono::steady_clock::time_point const deadline
    ) {
        return m_graph.ctx().waitForSubmittedFrames(deadline);
    }

    void requestFrameCapture() {
        if (m_capture_state == FrameCaptureState::Completed) {
            m_capture_state = FrameCaptureState::Ready;
        }
        if (m_capture_state == FrameCaptureState::Ready) {
            m_last_capture.reset();
            m_capture_requested = true;
        }
    }

    [[nodiscard]] FrameCaptureState captureState() const noexcept {
        return m_capture_state;
    }

    [[nodiscard]] std::optional<RendererFrameCapture> takeFrameCapture() {
        return std::exchange(m_last_capture, std::nullopt);
    }

    [[nodiscard]] RendererRuntimeInfo runtimeInfo() const {
        vk::Extent2d const extent = m_graph.ctx().extent();
        return RendererRuntimeInfo{
            .width = extent.x,
            .height = extent.y,
            .gpu_name = m_graph.ctx().deviceName(),
            .vulkan_api_version = m_graph.ctx().deviceVersion(),
            .validation_enabled = m_graph.ctx().validationEnabled(),
            .present_mode = rendererPresentMode(m_graph.ctx().presentMode()),
            .pipeline_path = m_mesh_shaders ? RendererPipelinePath::Mesh : RendererPipelinePath::Vertex,
            .cpu_frame_duration = m_cpu_frame_duration,
            .gpu_frame_duration = std::nullopt,
            .submitted_frame_count = m_submitted_frame_count,
        };
    }
private:
    void refreshCaptureState() {
        if (!m_options.enable_frame_capture) {
            m_capture_state = FrameCaptureState::Disabled;
            return;
        }
        if (!m_graph.ctx().swapchain().supportsTransferSource()) {
            m_capture_state = FrameCaptureState::UnsupportedSwapchainUsage;
            return;
        }
        if (!isSupportedCaptureFormat(m_graph.ctx().surfaceFormat())) {
            m_capture_state = FrameCaptureState::UnsupportedFormat;
            return;
        }
        m_capture_state = FrameCaptureState::Ready;
    }

    [[nodiscard]] uint64_t captureBufferSize() const noexcept {
        vk::Extent2d const extent = m_graph.ctx().extent();
        return static_cast<uint64_t>(extent.x) * static_cast<uint64_t>(extent.y) * 4U;
    }

    [[nodiscard]] bool prepareCaptureBuffer() {
        uint64_t const buffer_size = captureBufferSize();
        if (buffer_size == 0U || buffer_size > std::numeric_limits<VkDeviceSize>::max()) {
            CORE_ERROR("Renderer frame capture has an invalid extent");
            return false;
        }
        if (m_capture_buffer != VK_NULL_HANDLE && m_capture_buffer_size == buffer_size) {
            return true;
        }
        destroyCaptureBuffer();

        VkDevice const device = m_graph.ctx().device().handle();
        VkBufferCreateInfo const buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = static_cast<VkDeviceSize>(buffer_size),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (!VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &m_capture_buffer))) {
            m_capture_buffer = VK_NULL_HANDLE;
            CORE_ERROR("Failed to create the renderer frame capture buffer");
            return false;
        }

        VkMemoryRequirements requirements{ };
        vkGetBufferMemoryRequirements(device, m_capture_buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{ };
        vkGetPhysicalDeviceMemoryProperties(
            m_graph.ctx().physicalDevice().handle(),
            &memory_properties
        );

        uint32_t memory_type_index = memory_properties.memoryTypeCount;
        for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
            VkMemoryPropertyFlags const properties = memory_properties.memoryTypes[index].propertyFlags;
            if ((requirements.memoryTypeBits & (1U << index)) != 0U
                && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U
                && (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U
            ) {
                memory_type_index = index;
                break;
            }
        }
        if (memory_type_index == memory_properties.memoryTypeCount) {
            CORE_ERROR("No host-visible coherent memory type is available for renderer frame capture");
            destroyCaptureBuffer();
            return false;
        }

        VkMemoryAllocateInfo const allocation_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type_index,
        };
        if (!VK_CHECK(vkAllocateMemory(device, &allocation_info, nullptr, &m_capture_memory))) {
            CORE_ERROR("Failed to allocate renderer frame capture memory");
            destroyCaptureBuffer();
            return false;
        }
        if (!VK_CHECK(vkBindBufferMemory(device, m_capture_buffer, m_capture_memory, 0))) {
            CORE_ERROR("Failed to bind renderer frame capture memory");
            destroyCaptureBuffer();
            return false;
        }
        m_capture_buffer_size = buffer_size;
        return true;
    }

    void destroyCaptureBuffer() {
        if (m_graph.ctx().device().isNull()) {
            return;
        }
        VkDevice const device = m_graph.ctx().device().handle();
        if (m_capture_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, m_capture_buffer, nullptr);
            m_capture_buffer = VK_NULL_HANDLE;
        }
        if (m_capture_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, m_capture_memory, nullptr);
            m_capture_memory = VK_NULL_HANDLE;
        }
        m_capture_buffer_size = 0;
        m_capture_pending_readback = false;
    }

    void recordCapture(vk::FrameContext& frame) {
        if (!m_capture_requested || m_capture_state != FrameCaptureState::Ready
            || m_capture_buffer == VK_NULL_HANDLE
        ) {
            return;
        }

        frame.setImageBarrier(vk::ImageMemoryBarrier{
            .src_stages = vk::PipelineStage::BottomOfPipe,
            .dst_stages = vk::PipelineStage::Transfer,
            .src_access = vk::AccessFlags::None,
            .dst_access = vk::AccessFlag::TransferRead,
            .old_layout = vk::ImageLayout::PresentSrc,
            .new_layout = vk::ImageLayout::TransferSrcOptimal,
        });
        vk::Extent2d const extent = m_graph.ctx().extent();
        VkBufferImageCopy const copy{
            .imageSubresource = VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = VkExtent3D{
                .width = extent.x,
                .height = extent.y,
                .depth = 1,
            },
        };
        vkCmdCopyImageToBuffer(
            frame.commandBuffer().handle(),
            m_graph.ctx().swapchainImage().handle(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_capture_buffer,
            1,
            &copy
        );
        frame.setImageBarrier(vk::ImageMemoryBarrier{
            .src_stages = vk::PipelineStage::Transfer,
            .dst_stages = vk::PipelineStage::BottomOfPipe,
            .src_access = vk::AccessFlag::TransferRead,
            .dst_access = vk::AccessFlags::None,
            .old_layout = vk::ImageLayout::TransferSrcOptimal,
            .new_layout = vk::ImageLayout::PresentSrc,
        });
        m_capture_requested = false;
        m_capture_pending_readback = true;
    }

    void readCaptureBuffer() {
        void* mapped_data = nullptr;
        if (!VK_CHECK(vkMapMemory(
            m_graph.ctx().device().handle(),
            m_capture_memory,
            0,
            static_cast<VkDeviceSize>(m_capture_buffer_size),
            0,
            &mapped_data
        ))) {
            CORE_ERROR("Failed to map renderer frame capture memory");
            m_capture_state = FrameCaptureState::Failed;
            m_capture_pending_readback = false;
            return;
        }
        vk::Extent2d const extent = m_graph.ctx().extent();
        RendererFrameCapture capture{
            .width = extent.x,
            .height = extent.y,
            .srgb_encoded = isSrgbCaptureFormat(m_graph.ctx().surfaceFormat()),
            .rgba8 = std::vector<uint8_t>(m_capture_buffer_size),
        };
        uint8_t const* const source = static_cast<uint8_t const*>(mapped_data);
        for (uint64_t pixel = 0; pixel < m_capture_buffer_size / 4U; ++pixel) {
            uint64_t const offset = pixel * 4U;
            if (m_graph.ctx().surfaceFormat() == vk::Format::B8G8R8A8SRGB
                || m_graph.ctx().surfaceFormat() == vk::Format::B8G8R8A8UNorm
            ) {
                capture.rgba8[offset] = source[offset + 2U];
                capture.rgba8[offset + 1U] = source[offset + 1U];
                capture.rgba8[offset + 2U] = source[offset];
            } else {
                capture.rgba8[offset] = source[offset];
                capture.rgba8[offset + 1U] = source[offset + 1U];
                capture.rgba8[offset + 2U] = source[offset + 2U];
            }
            capture.rgba8[offset + 3U] = source[offset + 3U];
        }
        vkUnmapMemory(m_graph.ctx().device().handle(), m_capture_memory);
        m_last_capture = std::move(capture);
        m_capture_state = FrameCaptureState::Completed;
        m_capture_pending_readback = false;
    }

    void createPipelines() {
        vk::Device& dev = m_graph.ctx().device();
        m_mesh_shaders = detail::shouldUseMeshShaderPipelines(
            m_options.prefer_mesh_shaders,
            m_graph.ctx()
        );
        m_main_shader_stage = m_mesh_shaders ? vk::ShaderStage::Mesh : vk::ShaderStage::Vertex;
        if (!m_mesh_shaders) {
            CORE_INFO("Using vertex pipelines (mesh shaders unavailable, incompatible, or disabled)");
        } else {
            CORE_INFO("Using mesh shader pipelines");
        }

        vk::SpirV grid_shader = m_shader_assets.load(
            m_mesh_shaders ? "grid.mesh.spv" : "grid.vert.spv"
        );
        vk::SpirV player_shader = m_shader_assets.load(
            m_mesh_shaders ? "player.mesh.spv" : "player.vert.spv"
        );
        vk::SpirV trivial_frag = m_shader_assets.load("trivial.frag.spv");
        m_grid_shader = vk::ShaderModule{ dev, grid_shader };
        m_player_shader = vk::ShaderModule{ dev, player_shader };
        m_fragment_shader = vk::ShaderModule{ dev, trivial_frag };

        m_grid_layout = vk::PipelineLayout{
            dev,
            vk::PipelineLayout::Info::fromSpirVs(grid_shader, trivial_frag),
        };
        m_player_layout = vk::PipelineLayout{
            dev,
            vk::PipelineLayout::Info::fromSpirVs(player_shader, trivial_frag),
        };

        vk::GraphicsPipelineOptions grid_opts{
            .shaders = {
                {m_main_shader_stage,       grid_shader,  "main"},
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
                {m_main_shader_stage,       player_shader, "main"},
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
    ShaderAssets const& m_shader_assets;
    VulkanRendererOptions const m_options;
    vk::FrameGraph m_graph;
    vk::FramePassId m_render_pass;

    FrameCaptureState m_capture_state = FrameCaptureState::Disabled;
    std::optional<RendererFrameCapture> m_last_capture;
    std::chrono::nanoseconds m_cpu_frame_duration{ 0 };
    VkBuffer m_capture_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_capture_memory = VK_NULL_HANDLE;
    uint64_t m_capture_buffer_size = 0;
    bool m_capture_requested = false;
    bool m_capture_pending_readback = false;

    bool m_mesh_shaders = false;
    uint64_t m_submitted_frame_count = 0U;
    vk::ShaderStage m_main_shader_stage = vk::ShaderStage::Mesh;

    vk::ShaderModule m_grid_shader;
    vk::ShaderModule m_player_shader;
    vk::ShaderModule m_fragment_shader;
    vk::PipelineLayout m_grid_layout;
    vk::PipelineLayout m_player_layout;
    vk::GraphicsPipeline m_grid_pipeline;
    vk::GraphicsPipeline m_player_pipeline;
};

VulkanRenderer::VulkanRenderer(
    core::vk::SurfaceProvider const& surface_provider,
    ShaderAssets const& shader_assets,
    VulkanRendererOptions const options
)
    : m_impl(std::make_unique<Impl>(surface_provider, shader_assets, options))
{ }

VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::render(
    std::span<PlayerRenderData const> const players,
    std::chrono::steady_clock::time_point const deadline
) {
    return m_impl->render(players, deadline);
}

void VulkanRenderer::hotReload() {
    m_impl->hotReload();
}

void VulkanRenderer::waitIdle() {
    m_impl->waitIdle();
}

bool VulkanRenderer::waitForSubmittedFrames(
    std::chrono::steady_clock::time_point const deadline
) {
    return m_impl->waitForSubmittedFrames(deadline);
}

void VulkanRenderer::requestFrameCapture() {
    m_impl->requestFrameCapture();
}

FrameCaptureState VulkanRenderer::captureState() const {
    return m_impl->captureState();
}

std::optional<RendererFrameCapture> VulkanRenderer::takeFrameCapture() {
    return m_impl->takeFrameCapture();
}

RendererRuntimeInfo VulkanRenderer::runtimeInfo() const {
    return m_impl->runtimeInfo();
}

} // namespace client
