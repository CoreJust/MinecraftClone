#include <client/render/DepthFormat.hpp>
#include <client/render/VulkanRenderer.hpp>

#if defined(__ANDROID__)
#include <core/graphics/vulkan/android/AndroidSurface.hpp>
#else
#include <core/graphics/vulkan/glfw/GlfwSurface.hpp>
#endif

#include <core/kernel/Program.hpp>

#if !defined(__ANDROID__)
#include <core/platform/glfw/GlfwWindow.hpp>
#endif

#include <glm/mat4x4.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace client {
namespace {

#if (!defined(__ANDROID__) && defined(_CORE_DEBUG)) || defined(_MC_VK_VALIDATION_LAYERS)
constexpr bool REQUIRE_VALIDATION = true;
#else
constexpr bool REQUIRE_VALIDATION = false;
#endif

constexpr uint32_t GRID_WORKGROUPS_X = 32U;
constexpr uint32_t GRID_WORKGROUPS_Y = 32U;
constexpr uint32_t KERNEL_CACHE_CAPACITY = 8U;
constexpr uint32_t QUAD_VERTEX_COUNT = 6U;
constexpr uint32_t BOX_VERTEX_COUNT = 36U;
constexpr std::array DEPTH_FORMAT_CANDIDATES{
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D16_UNORM,
};

struct alignas(16) GridPushConstants final {
    glm::mat4 projection_view{ 1.0F };
    float world_size = 32.0F;
    float line_width = 0.03F;
    float pad0 = 0.0F;
    float pad1 = 0.0F;
    std::array<float, 4> line_color{ 0.16F, 0.16F, 0.18F, 1.0F };
};
static_assert(sizeof(GridPushConstants) == 96U);

struct alignas(16) BoxPushConstants final {
    glm::mat4 projection_view{ 1.0F };
    std::array<float, 4> origin{ 0.0F, 0.0F, 0.0F, 0.0F };
    std::array<float, 4> extent{ 2.0F, 2.0F, 2.0F, 0.0F };
    std::array<float, 4> color{ 1.0F, 1.0F, 1.0F, 1.0F };
};
static_assert(sizeof(BoxPushConstants) == 112U);

struct DebugHudPushConstants final {
    std::array<float, 2> resolution{ 0.0F, 0.0F };
    float scale = 1.0F;
    std::array<uint32_t, client::DEBUG_HUD_MAX_INSTANCES> packed_ascii{ };
};
static_assert(sizeof(DebugHudPushConstants) == 124U);

[[nodiscard]]
std::chrono::nanoseconds remaining(std::chrono::steady_clock::time_point const deadline)
{
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::seconds{ 5 };
    }
    auto const duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now()
    );
    return std::max(duration, std::chrono::nanoseconds::zero());
}

[[nodiscard]]
RendererPresentMode rendererPresentMode(VkPresentModeKHR const mode) noexcept
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return RendererPresentMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR: return RendererPresentMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR: return RendererPresentMode::FIFO;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return RendererPresentMode::FIFORelaxed;
    default: return RendererPresentMode::Unknown;
    }
}

void checkResult(VkResult const result, char const* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string{ operation } + " failed with Vulkan result " + std::to_string(result));
    }
}

void recordFlat3dScene(
    VkCommandBuffer const command,
    VkPipeline const grid_pipeline,
    VkPipelineLayout const grid_layout,
    VkPipeline const player_pipeline,
    VkPipelineLayout const player_layout,
    Camera const& camera,
    std::span<PlayerRenderData const> const players,
    VkExtent2D const extent
)
{
    std::optional<glm::mat4> const projection = camera.projectionMatrix(extent.width, extent.height);
    if (!projection.has_value()) {
        return;
    }
    glm::mat4 const projection_view = *projection * camera.viewMatrix();
    BoxPushConstants const platform_push{
        .projection_view = projection_view,
        .origin = { 0.0F, 0.0F, -1.0F, 0.0F },
        .extent = { 32.0F, 32.0F, 0.98F, 0.0F },
        .color = { 0.12F, 0.17F, 0.24F, 1.0F },
    };
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, player_pipeline);
    vkCmdPushConstants(
        command,
        player_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0U,
        sizeof(platform_push),
        &platform_push
    );
    vkCmdDraw(command, BOX_VERTEX_COUNT, 1U, 0U, 0U);

    GridPushConstants const grid_push{ .projection_view = projection_view };
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipeline);
    vkCmdPushConstants(
        command,
        grid_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0U,
        sizeof(grid_push),
        &grid_push
    );
    vkCmdDraw(command, QUAD_VERTEX_COUNT, GRID_WORKGROUPS_X * GRID_WORKGROUPS_Y, 0U, 0U);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, player_pipeline);
    for (PlayerRenderData const& player : players) {
        BoxPushConstants const push{
            .projection_view = projection_view,
            .origin = {
                player.x,
                player.y,
                0.0F,
                0.0F,
            },
            .extent = { 2.0F, 2.0F, 2.0F, 0.0F },
            .color = player.color,
        };
        vkCmdPushConstants(
            command,
            player_layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0U,
            sizeof(push),
            &push
        );
        vkCmdDraw(command, BOX_VERTEX_COUNT, 1U, 0U, 0U);
    }
}

} // namespace

struct VulkanRenderer::Impl final {
    using PresentationContext = core::graphics::vulkan::PresentationContext;
    using ResourceScope = PresentationContext::PresentationResourceScope;

    struct DepthTarget final {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool layout_initialized = false;
    };

    Impl(
        std::shared_ptr<PresentationContext> context,
        ShaderAssets const& shader_assets,
        VulkanRendererOptions options
    )
        : m_context(std::move(context))
        , m_shader_assets(shader_assets)
        , m_options(options)
    {
        if (!m_context) {
            throw std::invalid_argument("Vulkan renderer requires a presentation context");
        }
        if (m_context->info().surface_transform.requires_client_orientation_compensation) {
            throw std::runtime_error(
                "Vulkan renderer cannot present upright without client orientation compensation"
            );
        }
        createResources();
        refreshCaptureState();
    }

    ~Impl()
    {
        try {
            m_context->recreate(
                m_context->info().extent,
                {
                    .before = &Impl::beforeRecreate,
                    .user_data = this,
                },
                std::chrono::seconds{ 5 }
            );
        } catch (std::exception const&) {
            destroyResources();
        }
    }

    [[nodiscard]] bool render(
        std::span<PlayerRenderData const> const players,
        DebugHudInput debug_hud_input,
        float const debug_hud_dpi_scale,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        debug_hud_input.presented = m_last_presented;
        m_debug_hud_state.setDpiScale(debug_hud_dpi_scale);
        m_debug_hud_state.update(debug_hud_input);
        m_debug_hud_batch.size = 0U;
        if (m_debug_hud_state.buildBatch(m_debug_hud_text, m_debug_hud_batch)) {
            DebugHudSnapshot const snapshot = m_debug_hud_state.snapshot();
            m_debug_hud_push.resolution = {
                static_cast<float>(m_context->info().extent.width),
                static_cast<float>(m_context->info().extent.height),
            };
            m_debug_hud_push.scale = snapshot.dpi_scale;
            for (size_t index = 0U; index < m_debug_hud_batch.size; ++index) {
                m_debug_hud_push.packed_ascii[index] = m_debug_hud_batch.instances[index].packed_ascii;
            }
        }

        std::chrono::steady_clock::time_point const acquire_started_at = std::chrono::steady_clock::now();
        std::optional<PresentationContext::Frame> frame;
        core::graphics::vulkan::PresentationAcquireResult const acquired = m_context->acquire(
            remaining(deadline),
            frame
        );
        m_cpu_acquire_wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - acquire_started_at
        );
        if (acquired == core::graphics::vulkan::PresentationAcquireResult::NeedsRecreation) {
            m_last_presented = false;
            recreate(m_context->info().extent, deadline);
            return false;
        }
        if (acquired != core::graphics::vulkan::PresentationAcquireResult::Ready || !frame.has_value()) {
            m_last_presented = false;
            return false;
        }

        std::chrono::steady_clock::time_point const command_record_started_at =
            std::chrono::steady_clock::now();
        m_players = players;
        m_image_view = frame->imageView();
        m_current_depth_target = &depthTargetFor(frame->image());
        frame->record(&Impl::recordFrame, this);
        m_cpu_command_record_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - command_record_started_at
        );
        std::chrono::steady_clock::time_point const complete_present_started_at =
            std::chrono::steady_clock::now();
        m_context->complete(*frame);
        m_cpu_complete_present_wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - complete_present_started_at
        );
        m_current_depth_target = nullptr;
        m_last_presented = true;
        m_cpu_frame_duration = m_cpu_command_record_duration + m_cpu_complete_present_wait_duration;
        ++m_submitted_frame_count;

        if (m_capture_requested) {
            std::optional<std::span<uint8_t const>> const readback = m_context->takeCompletedReadback(
                remaining(deadline)
            );
            if (readback.has_value()) {
                core::graphics::vulkan::PresentationInfo const& info = m_context->info();
                m_last_capture = RendererFrameCapture{
                    .width = info.extent.width,
                    .height = info.extent.height,
                    .srgb_encoded = info.srgb,
                    .rgba8 = std::vector<uint8_t>(readback->begin(), readback->end()),
                };
                m_capture_requested = false;
                m_capture_state = FrameCaptureState::Completed;
            }
        }
        return true;
    }

    void setDebugHudEnabled(bool const enabled) noexcept
    {
        m_debug_hud_state.setEnabled(enabled);
    }

    void toggleDebugHud() noexcept
    {
        m_debug_hud_state.toggle();
    }

    void setCamera(CameraPose const pose) noexcept
    {
        static_cast<void>(m_camera.setPosition(pose.position));
        static_cast<void>(m_camera.setAngles(pose.angles));
    }

    void hotReload()
    {
        recreate(m_context->info().extent, std::chrono::steady_clock::time_point::max());
    }

    void resize(uint32_t const width, uint32_t const height)
    {
        VkExtent2D const extent{ .width = width, .height = height };
        if (extent.width != m_context->info().extent.width || extent.height != m_context->info().extent.height) {
            recreate(extent, std::chrono::steady_clock::time_point::max());
        }
    }

    [[nodiscard]] bool waitForSubmittedFrames(std::chrono::steady_clock::time_point const deadline)
    {
        try {
            m_context->recreate(
                m_context->info().extent,
                {
                    .before = &Impl::beforeRecreate,
                    .after = &Impl::afterRecreate,
                    .user_data = this,
                },
                remaining(deadline)
            );
            return true;
        } catch (std::exception const&) {
            return false;
        }
    }

    void requestFrameCapture()
    {
        if (m_capture_state == FrameCaptureState::Completed) {
            m_capture_state = FrameCaptureState::Ready;
        }
        if (m_capture_state == FrameCaptureState::Ready) {
            m_last_capture.reset();
            m_capture_requested = true;
        }
    }

    [[nodiscard]] FrameCaptureState captureState() const noexcept { return m_capture_state; }

    [[nodiscard]] std::optional<RendererFrameCapture> takeFrameCapture()
    {
        return std::exchange(m_last_capture, std::nullopt);
    }

    [[nodiscard]] RendererRuntimeInfo runtimeInfo() const
    {
        core::graphics::vulkan::PresentationInfo const& info = m_context->info();
        return RendererRuntimeInfo{
            .width = info.extent.width,
            .height = info.extent.height,
            .gpu_name = "presentation-context",
            .vulkan_api_version = {
                .major = VK_VERSION_MAJOR(info.api_version),
                .minor = VK_VERSION_MINOR(info.api_version),
                .patch = VK_VERSION_PATCH(info.api_version),
            },
            .validation_enabled = info.validation_enabled,
            .present_mode = rendererPresentMode(info.present_mode),
            .pipeline_path = RendererPipelinePath::Vertex,
            .cpu_acquire_wait_duration = m_cpu_acquire_wait_duration,
            .cpu_command_record_duration = m_cpu_command_record_duration,
            .cpu_complete_present_wait_duration = m_cpu_complete_present_wait_duration,
            .cpu_frame_duration = m_cpu_frame_duration,
            .gpu_frame_duration = std::nullopt,
            .submitted_frame_count = m_submitted_frame_count,
            .debug_hud_draw_count = m_debug_hud_draw_count,
        };
    }

    void recreate(VkExtent2D const extent, std::chrono::steady_clock::time_point const deadline)
    {
        m_context->recreate(
            extent,
            {
                .before = &Impl::beforeRecreate,
                .after = &Impl::afterRecreate,
                .user_data = this,
            },
            remaining(deadline)
        );
    }

private:
    static void beforeRecreate(void* const user_data)
    {
        static_cast<Impl*>(user_data)->destroyResources();
    }

    static void afterRecreate(
        core::graphics::vulkan::PresentationInfo const&,
        void* const user_data
    )
    {
        Impl& self = *static_cast<Impl*>(user_data);
        self.createResources();
        self.refreshCaptureState();
    }

    static void recordFrame(VkCommandBuffer const command, void* const user_data)
    {
        static_cast<Impl*>(user_data)->record(command);
    }

    void record(VkCommandBuffer const command)
    {
        m_debug_hud_draw_count = 0U;
        DepthTarget& depth_target = *m_current_depth_target;
        if (!depth_target.layout_initialized) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barrier.srcAccessMask = 0U;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.image = depth_target.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier.subresourceRange.levelCount = 1U;
            barrier.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(
                command,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0U,
                0U,
                nullptr,
                0U,
                nullptr,
                1U,
                &barrier
            );
            depth_target.layout_initialized = true;
        }
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = m_image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color.float32[0] = 0.28F;
        color_attachment.clearValue.color.float32[1] = 0.48F;
        color_attachment.clearValue.color.float32[2] = 0.72F;
        color_attachment.clearValue.color.float32[3] = 1.0F;
        VkExtent2D const extent = m_resources->extent();
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth_target.view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil.depth = 1.0F;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1U;
        rendering.colorAttachmentCount = 1U;
        rendering.pColorAttachments = &color_attachment;
        rendering.pDepthAttachment = &depth_attachment;
        m_begin_rendering(command, &rendering);
        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{};
        scissor.extent = extent;
        vkCmdSetViewport(command, 0U, 1U, &viewport);
        vkCmdSetScissor(command, 0U, 1U, &scissor);

        recordFlat3dScene(
            command,
            m_grid_pipeline,
            m_grid_layout,
            m_player_pipeline,
            m_player_layout,
            m_camera,
            m_players,
            extent
        );
        if (m_debug_hud_batch.size > 0U) {
            m_debug_hud_draw_count = 1U;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debug_hud_pipeline);
            vkCmdPushConstants(
                command,
                m_debug_hud_layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0U,
                sizeof(m_debug_hud_push),
                &m_debug_hud_push
            );
            vkCmdDraw(
                command,
                QUAD_VERTEX_COUNT,
                static_cast<uint32_t>(m_debug_hud_batch.size),
                0U,
                0U
            );
        }
        m_end_rendering(command);
    }

    [[nodiscard]] VkPipelineLayout createLayout(uint32_t const push_constant_size) const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0U;
        range.size = push_constant_size;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        checkResult(vkCreatePipelineLayout(m_resources->device(), &info, nullptr, &layout), "vkCreatePipelineLayout");
        return layout;
    }

    void createResources()
    {
        m_resources.emplace(m_context->resources());
        auto const dynamic_rendering = m_resources->dynamicRenderingCommands();
        m_begin_rendering = dynamic_rendering.begin;
        m_end_rendering = dynamic_rendering.end;
        if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
            throw std::runtime_error("Vulkan presentation device does not expose dynamic rendering commands");
        }
        selectDepthFormat();
        auto const grid = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("grid.vert.spv")
        );
        auto const player = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("player.vert.spv")
        );
        auto const debug_hud = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("debug_hud.vert.spv")
        );
        auto const fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("trivial.frag.spv")
        );
        auto const debug_hud_fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("debug_hud.frag.spv")
        );
        m_grid_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = grid,
                .entrypoint = "main",
                .required_bindings = {},
            },
            {
                .module = fragment,
                .entrypoint = "main",
                .required_bindings = {},
            }
        ));
        m_player_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = player,
                .entrypoint = "main",
                .required_bindings = {},
            },
            {
                .module = fragment,
                .entrypoint = "main",
                .required_bindings = {},
            }
        ));
        m_debug_hud_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = debug_hud,
                .entrypoint = "main",
                .required_bindings = {},
            },
            {
                .module = debug_hud_fragment,
                .entrypoint = "main",
                .required_bindings = {},
            }
        ));
        m_grid_layout = createLayout(static_cast<uint32_t>(sizeof(GridPushConstants)));
        m_player_layout = createLayout(static_cast<uint32_t>(sizeof(BoxPushConstants)));
        m_debug_hud_layout = createLayout(static_cast<uint32_t>(sizeof(DebugHudPushConstants)));
        m_kernel_cache.emplace(KERNEL_CACHE_CAPACITY);
        core::graphics::vulkan::VulkanDeviceReference const device = m_resources->deviceReference();
        m_grid_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_grid_program,
            {
                .layout = m_grid_layout,
                .color_format = m_resources->format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = VK_COMPARE_OP_LESS,
            }
        );
        m_player_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_player_program,
            {
                .layout = m_player_layout,
                .color_format = m_resources->format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = VK_COMPARE_OP_LESS,
            }
        );
        m_debug_hud_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_debug_hud_program,
            {
                .layout = m_debug_hud_layout,
                .color_format = m_resources->format(),
            }
        );
    }

    void destroyResources() noexcept
    {
        if (!m_resources.has_value()) {
            return;
        }
        VkDevice const device = m_resources->device();
        destroyDepthTargets(device);
        if (m_kernel_cache.has_value()) {
            m_kernel_cache->invalidate(m_resources->deviceReference().identity());
            m_kernel_cache.reset();
        }
        if (m_grid_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_grid_layout, nullptr); }
        if (m_player_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_player_layout, nullptr); }
        if (m_debug_hud_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_debug_hud_layout, nullptr);
        }
        m_grid_pipeline = VK_NULL_HANDLE;
        m_player_pipeline = VK_NULL_HANDLE;
        m_debug_hud_pipeline = VK_NULL_HANDLE;
        m_grid_layout = VK_NULL_HANDLE;
        m_player_layout = VK_NULL_HANDLE;
        m_debug_hud_layout = VK_NULL_HANDLE;
        m_grid_program.reset();
        m_player_program.reset();
        m_debug_hud_program.reset();
        m_begin_rendering = nullptr;
        m_end_rendering = nullptr;
        m_resources.reset();
    }

    void selectDepthFormat()
    {
        std::array<DepthFormatSupport, DEPTH_FORMAT_CANDIDATES.size()> supported{};
        for (size_t index = 0U; index < DEPTH_FORMAT_CANDIDATES.size(); ++index) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(
                m_resources->physicalDevice(),
                DEPTH_FORMAT_CANDIDATES[index],
                &properties
            );
            supported[index] = {
                .format = DEPTH_FORMAT_CANDIDATES[index],
                .depth_attachment_supported = (properties.optimalTilingFeatures
                    & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U,
            };
        }
        std::optional<VkFormat> const selected = selectDepthAttachmentFormat(supported);
        if (!selected.has_value()) {
            throw std::runtime_error("presentation device has no supported depth attachment format");
        }
        m_depth_format = *selected;
    }

    [[nodiscard]] DepthTarget& depthTargetFor(VkImage const color_image)
    {
        bool const needs_creation = m_depth_target_selection.select(color_image);
        auto const existing = m_depth_targets.find(color_image);
        if (existing != m_depth_targets.end()) {
            if (needs_creation) {
                throw std::logic_error("depth target selection is inconsistent with its resources");
            }
            return existing->second;
        }
        if (!needs_creation) {
            throw std::logic_error("depth target resource is missing for an acquired swapchain image");
        }
        auto const [created, inserted] = m_depth_targets.emplace(color_image, DepthTarget{});
        try {
            createDepthTarget(created->second);
        } catch (std::exception const&) {
            m_depth_targets.erase(created);
            m_depth_target_selection.remove(color_image);
            throw;
        }
        return created->second;
    }

    void createDepthTarget(DepthTarget& target)
    {
        VkDevice const device = m_resources->device();
        try {
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = m_depth_format;
            image_info.extent = { .width = m_resources->extent().width, .height = m_resources->extent().height, .depth = 1U };
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            checkResult(vkCreateImage(device, &image_info, nullptr, &target.image), "vkCreateImage depth");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, target.image, &requirements);
            VkPhysicalDeviceMemoryProperties memory_properties{};
            vkGetPhysicalDeviceMemoryProperties(m_resources->physicalDevice(), &memory_properties);
            uint32_t memory_type = memory_properties.memoryTypeCount;
            for (uint32_t index = 0U; index < memory_properties.memoryTypeCount; ++index) {
                if ((requirements.memoryTypeBits & (1U << index)) != 0U
                    && (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                    memory_type = index;
                    break;
                }
            }
            if (memory_type == memory_properties.memoryTypeCount) {
                throw std::runtime_error("presentation device has no local depth-image memory type");
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type;
            checkResult(vkAllocateMemory(device, &allocation, nullptr, &target.memory), "vkAllocateMemory depth");
            checkResult(vkBindImageMemory(device, target.image, target.memory, 0U), "vkBindImageMemory depth");
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = target.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = m_depth_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            checkResult(vkCreateImageView(device, &view_info, nullptr, &target.view), "vkCreateImageView depth");
            target.layout_initialized = false;
        } catch (std::exception const&) {
            destroyDepthTarget(device, target);
            throw;
        }
    }

    static void destroyDepthTarget(VkDevice const device, DepthTarget& target) noexcept
    {
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.view, nullptr);
        }
        if (target.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, target.image, nullptr);
        }
        if (target.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, target.memory, nullptr);
        }
        target.view = VK_NULL_HANDLE;
        target.image = VK_NULL_HANDLE;
        target.memory = VK_NULL_HANDLE;
        target.layout_initialized = false;
    }

    void destroyDepthTargets(VkDevice const device) noexcept
    {
        for (auto& [color_image, target] : m_depth_targets) {
            static_cast<void>(color_image);
            destroyDepthTarget(device, target);
        }
        m_depth_targets.clear();
        m_depth_target_selection.clear();
        m_current_depth_target = nullptr;
    }

    void refreshCaptureState() noexcept
    {
        if (m_last_capture.has_value()) {
            m_capture_state = FrameCaptureState::Completed;
            return;
        }
        m_capture_state = m_options.enable_frame_capture && m_context->info().transfer_source_enabled
            ? FrameCaptureState::Ready
            : m_options.enable_frame_capture
                ? FrameCaptureState::UnsupportedSwapchainUsage
                : FrameCaptureState::Disabled;
    }

    std::shared_ptr<PresentationContext> m_context;
    ShaderAssets const& m_shader_assets;
    VulkanRendererOptions m_options;
    std::optional<ResourceScope> m_resources;
    std::optional<core::graphics::vulkan::VulkanKernelCache> m_kernel_cache;
    std::span<PlayerRenderData const> m_players;
    Camera m_camera{
        { .position = { 16.0, -20.0, 22.0 }, .angles = { .pitch_degrees = -35.0 } },
    };
    VkImageView m_image_view = VK_NULL_HANDLE;
    VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
    std::unordered_map<VkImage, DepthTarget> m_depth_targets;
    DepthTargetSelection m_depth_target_selection;
    DepthTarget* m_current_depth_target = nullptr;
    std::optional<core::kernel::GraphicsProgram> m_grid_program;
    std::optional<core::kernel::GraphicsProgram> m_player_program;
    std::optional<core::kernel::GraphicsProgram> m_debug_hud_program;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_debug_hud_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
    VkPipeline m_debug_hud_pipeline = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR m_begin_rendering = nullptr;
    PFN_vkCmdEndRenderingKHR m_end_rendering = nullptr;
    FrameCaptureState m_capture_state = FrameCaptureState::Disabled;
    std::optional<RendererFrameCapture> m_last_capture;
    std::chrono::nanoseconds m_cpu_acquire_wait_duration{ 0 };
    std::chrono::nanoseconds m_cpu_command_record_duration{ 0 };
    std::chrono::nanoseconds m_cpu_complete_present_wait_duration{ 0 };
    std::chrono::nanoseconds m_cpu_frame_duration{ 0 };
    uint64_t m_submitted_frame_count = 0U;
    bool m_capture_requested = false;
    DebugHudState m_debug_hud_state;
    DebugHudText m_debug_hud_text;
    DebugHudBatch m_debug_hud_batch;
    DebugHudPushConstants m_debug_hud_push;
    bool m_last_presented = false;
    uint32_t m_debug_hud_draw_count = 0U;
};

namespace {

constexpr uint32_t OFFSCREEN_WIDTH = 640U;
constexpr uint32_t OFFSCREEN_HEIGHT = 480U;

class OffscreenDepthTarget final {
public:
    OffscreenDepthTarget(
        std::shared_ptr<core::graphics::vulkan::VulkanDevice const> device,
        VkFormat const format,
        VkExtent2D const extent
    )
        : m_device(std::move(device))
        , m_format(format)
    {
        try {
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = m_format;
            image_info.extent = { .width = extent.width, .height = extent.height, .depth = 1U };
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            checkResult(vkCreateImage(m_device->handle(), &image_info, nullptr, &m_image), "vkCreateImage offscreen depth");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_device->handle(), m_image, &requirements);
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(m_device->physicalDevice(), &properties);
            uint32_t memory_type = properties.memoryTypeCount;
            for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
                if ((requirements.memoryTypeBits & (1U << index)) != 0U
                    && (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                    memory_type = index;
                    break;
                }
            }
            if (memory_type == properties.memoryTypeCount) {
                throw std::runtime_error("offscreen device has no local depth-image memory type");
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type;
            checkResult(vkAllocateMemory(m_device->handle(), &allocation, nullptr, &m_memory), "vkAllocateMemory offscreen depth");
            checkResult(vkBindImageMemory(m_device->handle(), m_image, m_memory, 0U), "vkBindImageMemory offscreen depth");
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = m_image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = m_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            checkResult(vkCreateImageView(m_device->handle(), &view_info, nullptr, &m_view), "vkCreateImageView offscreen depth");
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~OffscreenDepthTarget() { reset(); }
    OffscreenDepthTarget(OffscreenDepthTarget const&) = delete;
    OffscreenDepthTarget& operator=(OffscreenDepthTarget const&) = delete;

    [[nodiscard]] VkImage image() const noexcept { return m_image; }
    [[nodiscard]] VkImageView view() const noexcept { return m_view; }
    [[nodiscard]] VkFormat format() const noexcept { return m_format; }
private:
    void reset() noexcept
    {
        if (!m_device || m_device->handle() == VK_NULL_HANDLE) {
            return;
        }
        if (m_view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device->handle(), m_view, nullptr);
        }
        if (m_image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device->handle(), m_image, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device->handle(), m_memory, nullptr);
        }
        m_view = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    std::shared_ptr<core::graphics::vulkan::VulkanDevice const> m_device;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
};

class OffscreenDrawResources final {
public:
    OffscreenDrawResources(
        std::shared_ptr<core::graphics::vulkan::VulkanDevice const> device,
        ShaderAssets const& shader_assets
    )
        : m_device(std::move(device))
        , m_depth_format(selectDepthFormat())
        , m_depth(m_device, m_depth_format, { .width = OFFSCREEN_WIDTH, .height = OFFSCREEN_HEIGHT })
    {
        try {
            auto const dynamic_rendering = m_device->dynamicRenderingCommands();
            m_begin_rendering = dynamic_rendering.begin;
            m_end_rendering = dynamic_rendering.end;
            if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
                throw std::runtime_error("offscreen device does not expose dynamic rendering commands");
            }
            auto const grid = std::make_shared<core::kernel::SpirvModule const>(shader_assets.load("grid.vert.spv"));
            auto const player = std::make_shared<core::kernel::SpirvModule const>(shader_assets.load("player.vert.spv"));
            auto const fragment = std::make_shared<core::kernel::SpirvModule const>(shader_assets.load("trivial.frag.spv"));
            m_grid_program.emplace(core::kernel::GraphicsProgram::create(
                { .module = grid, .entrypoint = "main", .required_bindings = {} },
                { .module = fragment, .entrypoint = "main", .required_bindings = {} }
            ));
            m_player_program.emplace(core::kernel::GraphicsProgram::create(
                { .module = player, .entrypoint = "main", .required_bindings = {} },
                { .module = fragment, .entrypoint = "main", .required_bindings = {} }
            ));
            m_grid_layout = createLayout(static_cast<uint32_t>(sizeof(GridPushConstants)));
            m_player_layout = createLayout(static_cast<uint32_t>(sizeof(BoxPushConstants)));
            core::graphics::vulkan::VulkanDeviceReference const reference = m_device->reference();
            m_grid_pipeline = m_cache.pipelineFor(reference, *m_grid_program, pipelineDescriptor(m_grid_layout));
            m_player_pipeline = m_cache.pipelineFor(reference, *m_player_program, pipelineDescriptor(m_player_layout));
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~OffscreenDrawResources()
    {
        reset();
    }

    OffscreenDrawResources(OffscreenDrawResources const&) = delete;
    OffscreenDrawResources& operator=(OffscreenDrawResources const&) = delete;

    [[nodiscard]] OffscreenDepthTarget const& depth() const noexcept { return m_depth; }
    [[nodiscard]] VkPipeline gridPipeline() const noexcept { return m_grid_pipeline; }
    [[nodiscard]] VkPipelineLayout gridLayout() const noexcept { return m_grid_layout; }
    [[nodiscard]] VkPipeline playerPipeline() const noexcept { return m_player_pipeline; }
    [[nodiscard]] VkPipelineLayout playerLayout() const noexcept { return m_player_layout; }
    [[nodiscard]] PFN_vkCmdBeginRenderingKHR beginRendering() const noexcept { return m_begin_rendering; }
    [[nodiscard]] PFN_vkCmdEndRenderingKHR endRendering() const noexcept { return m_end_rendering; }
private:
    void reset() noexcept
    {
        if (!m_device || m_device->handle() == VK_NULL_HANDLE) {
            return;
        }
        m_cache.invalidate(m_device->identity());
        if (m_grid_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device->handle(), m_grid_layout, nullptr);
        }
        if (m_player_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device->handle(), m_player_layout, nullptr);
        }
        m_grid_layout = VK_NULL_HANDLE;
        m_player_layout = VK_NULL_HANDLE;
        m_grid_pipeline = VK_NULL_HANDLE;
        m_player_pipeline = VK_NULL_HANDLE;
    }

    [[nodiscard]] VkFormat selectDepthFormat() const
    {
        std::array<DepthFormatSupport, DEPTH_FORMAT_CANDIDATES.size()> supported{};
        for (size_t index = 0U; index < DEPTH_FORMAT_CANDIDATES.size(); ++index) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(m_device->physicalDevice(), DEPTH_FORMAT_CANDIDATES[index], &properties);
            supported[index] = {
                .format = DEPTH_FORMAT_CANDIDATES[index],
                .depth_attachment_supported = (
                    properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                ) != 0U,
            };
        }
        std::optional<VkFormat> const selected = selectDepthAttachmentFormat(supported);
        if (!selected.has_value()) {
            throw std::runtime_error("offscreen device has no supported depth attachment format");
        }
        return *selected;
    }

    [[nodiscard]] VkPipelineLayout createLayout(uint32_t const size) const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = size;
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(vkCreatePipelineLayout(m_device->handle(), &info, nullptr, &layout), "vkCreatePipelineLayout offscreen");
        return layout;
    }

    [[nodiscard]] core::graphics::vulkan::PipelineDescriptor pipelineDescriptor(VkPipelineLayout const layout) const noexcept
    {
        return {
            .layout = layout,
            .color_format = VK_FORMAT_R8G8B8A8_UNORM,
            .depth_format = m_depth_format,
            .depth_test_enabled = true,
            .depth_write_enabled = true,
            .depth_compare_op = VK_COMPARE_OP_LESS,
        };
    }

    std::shared_ptr<core::graphics::vulkan::VulkanDevice const> m_device;
    VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
    OffscreenDepthTarget m_depth;
    core::graphics::vulkan::VulkanKernelCache m_cache{ KERNEL_CACHE_CAPACITY };
    std::optional<core::kernel::GraphicsProgram> m_grid_program;
    std::optional<core::kernel::GraphicsProgram> m_player_program;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR m_begin_rendering = nullptr;
    PFN_vkCmdEndRenderingKHR m_end_rendering = nullptr;
};

} // namespace

struct VulkanOffscreenRenderer::Impl final {
    explicit Impl(ShaderAssets const& shader_assets, bool const require_validation)
        : m_instance(core::graphics::vulkan::VulkanInstance::create({}, REQUIRE_VALIDATION || require_validation))
        , m_device(core::graphics::vulkan::VulkanDevice::create(m_instance))
        , m_resources(std::make_shared<OffscreenDrawResources>(m_device, shader_assets))
        , m_target(std::make_unique<core::graphics::vulkan::VulkanOffscreenTarget>(
            m_device,
            core::graphics::TextureDescriptor{ .width = OFFSCREEN_WIDTH, .height = OFFSCREEN_HEIGHT }
        ))
    { }

    [[nodiscard]] RendererFrameCapture render(
        std::span<PlayerRenderData const> const players,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        m_players = players;
        m_target->recordAndReadback(m_resources, &Impl::record, this, remaining(deadline));
        std::span<uint8_t const> const readback = m_target->readback();
        return {
            .width = OFFSCREEN_WIDTH,
            .height = OFFSCREEN_HEIGHT,
            .srgb_encoded = false,
            .rgba8 = std::vector<uint8_t>(readback.begin(), readback.end()),
        };
    }

    [[nodiscard]] bool validationEnabled() const noexcept { return m_instance->validationEnabled(); }
    [[nodiscard]] uint32_t validationErrorCount() const noexcept { return m_instance->validationErrorCount(); }
private:
    static void record(core::graphics::vulkan::VulkanOffscreenTarget::Recording const& recording, void* const user_data)
    {
        Impl& self = *static_cast<Impl*>(user_data);
        OffscreenDrawResources const& resources = *self.m_resources;
        if (!self.m_depth_initialized) {
            VkImageMemoryBarrier depth_barrier{};
            depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depth_barrier.image = resources.depth().image();
            depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth_barrier.subresourceRange.levelCount = 1U;
            depth_barrier.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(
                recording.command_buffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0U,
                0U,
                nullptr,
                0U,
                nullptr,
                1U,
                &depth_barrier
            );
            self.m_depth_initialized = true;
        }
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = recording.color_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.float32[0] = 0.28F;
        color.clearValue.color.float32[1] = 0.48F;
        color.clearValue.color.float32[2] = 0.72F;
        color.clearValue.color.float32[3] = 1.0F;
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = resources.depth().view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil.depth = 1.0F;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = recording.extent;
        info.layerCount = 1U;
        info.colorAttachmentCount = 1U;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        resources.beginRendering()(recording.command_buffer, &info);
        VkViewport viewport{};
        viewport.width = static_cast<float>(recording.extent.width);
        viewport.height = static_cast<float>(recording.extent.height);
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{};
        scissor.extent = recording.extent;
        vkCmdSetViewport(recording.command_buffer, 0U, 1U, &viewport);
        vkCmdSetScissor(recording.command_buffer, 0U, 1U, &scissor);
        recordFlat3dScene(
            recording.command_buffer,
            resources.gridPipeline(),
            resources.gridLayout(),
            resources.playerPipeline(),
            resources.playerLayout(),
            self.m_camera,
            self.m_players,
            recording.extent
        );
        resources.endRendering()(recording.command_buffer);
    }

    std::shared_ptr<core::graphics::vulkan::VulkanInstance> m_instance;
    std::shared_ptr<core::graphics::vulkan::VulkanDevice> m_device;
    std::shared_ptr<OffscreenDrawResources> m_resources;
    std::unique_ptr<core::graphics::vulkan::VulkanOffscreenTarget> m_target;
    std::span<PlayerRenderData const> m_players;
    bool m_depth_initialized = false;
    Camera m_camera{
        { .position = { 16.0, -20.0, 22.0 }, .angles = { .pitch_degrees = -35.0 } },
    };
};

#if !defined(__ANDROID__)
std::shared_ptr<core::graphics::vulkan::PresentationContext> VulkanRenderer::createPresentationContext(
    core::platform::glfw::GlfwWindow const& window,
    VulkanRendererOptions const options
)
{
    std::shared_ptr<core::graphics::vulkan::VulkanInstance> const instance =
        core::graphics::vulkan::VulkanInstance::create(
            core::graphics::vulkan::glfw::requiredInstanceExtensions(),
            REQUIRE_VALIDATION || options.require_validation
        );
    uint32_t width = 0U;
    uint32_t height = 0U;
    window.framebufferSize(width, height);
    return core::graphics::vulkan::PresentationContext::create(
        core::graphics::vulkan::glfw::createSurface(instance, window),
        {
            .enable_validation = REQUIRE_VALIDATION || options.require_validation,
            .prefer_mesh_shaders = false,
            .enable_transfer_source = options.enable_frame_capture,
            .require_transfer_source = options.enable_frame_capture,
            .prefer_immediate_present = options.require_immediate_present_mode,
            .require_immediate_present = options.require_immediate_present_mode,
        },
        { .width = width, .height = height }
    );
}
#endif

#if defined(__ANDROID__)
std::shared_ptr<core::graphics::vulkan::PresentationContext> VulkanRenderer::createPresentationContext(
    ANativeWindow* const window,
    uint32_t const width,
    uint32_t const height,
    VulkanRendererOptions const options
)
{
    std::shared_ptr<core::graphics::vulkan::VulkanInstance> const instance =
        core::graphics::vulkan::VulkanInstance::create(
            core::graphics::vulkan::android::requiredInstanceExtensions(),
            REQUIRE_VALIDATION || options.require_validation
        );
    return core::graphics::vulkan::PresentationContext::create(
        core::graphics::vulkan::android::createSurface(instance, window),
        {
            .enable_validation = REQUIRE_VALIDATION || options.require_validation,
            .prefer_mesh_shaders = false,
            .enable_transfer_source = options.enable_frame_capture,
            .require_transfer_source = options.enable_frame_capture,
            .prefer_immediate_present = options.require_immediate_present_mode,
            .require_immediate_present = options.require_immediate_present_mode,
        },
        { .width = width, .height = height }
    );
}
#endif

VulkanRenderer::VulkanRenderer(
    std::shared_ptr<core::graphics::vulkan::PresentationContext> context,
    ShaderAssets const& shader_assets,
    VulkanRendererOptions const options
)
    : m_impl(std::make_unique<Impl>(std::move(context), shader_assets, options))
{ }

VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::render(std::span<PlayerRenderData const> const players, std::chrono::steady_clock::time_point const deadline)
{
    return render(players, DebugHudInput{}, 1.0F, deadline);
}

bool VulkanRenderer::render(
    std::span<PlayerRenderData const> const players,
    DebugHudInput const debug_hud_input,
    float const debug_hud_dpi_scale,
    std::chrono::steady_clock::time_point const deadline
)
{
    return m_impl->render(players, debug_hud_input, debug_hud_dpi_scale, deadline);
}

void VulkanRenderer::setDebugHudEnabled(bool const enabled) noexcept
{
    m_impl->setDebugHudEnabled(enabled);
}

void VulkanRenderer::toggleDebugHud() noexcept
{
    m_impl->toggleDebugHud();
}

void VulkanRenderer::setCamera(CameraPose const pose) noexcept
{
    m_impl->setCamera(pose);
}

void VulkanRenderer::hotReload()
{
    m_impl->hotReload();
}

void VulkanRenderer::recreate(uint32_t const width, uint32_t const height)
{
    m_impl->resize(width, height);
}

bool VulkanRenderer::waitForSubmittedFrames(std::chrono::steady_clock::time_point const deadline)
{
    return m_impl->waitForSubmittedFrames(deadline);
}

void VulkanRenderer::requestFrameCapture()
{
    m_impl->requestFrameCapture();
}

FrameCaptureState VulkanRenderer::captureState() const
{
    return m_impl->captureState();
}

std::optional<RendererFrameCapture> VulkanRenderer::takeFrameCapture()
{
    return m_impl->takeFrameCapture();
}

RendererRuntimeInfo VulkanRenderer::runtimeInfo() const
{
    return m_impl->runtimeInfo();
}

VulkanOffscreenRenderer::VulkanOffscreenRenderer(
    ShaderAssets const& shader_assets,
    bool const require_validation
)
    : m_impl(std::make_unique<Impl>(shader_assets, require_validation))
{ }

VulkanOffscreenRenderer::~VulkanOffscreenRenderer() = default;

RendererFrameCapture VulkanOffscreenRenderer::render(
    std::span<PlayerRenderData const> const players,
    std::chrono::steady_clock::time_point const deadline
)
{
    return m_impl->render(players, deadline);
}

bool VulkanOffscreenRenderer::validationEnabled() const noexcept
{
    return m_impl->validationEnabled();
}

uint32_t VulkanOffscreenRenderer::validationErrorCount() const noexcept
{
    return m_impl->validationErrorCount();
}

} // namespace client
