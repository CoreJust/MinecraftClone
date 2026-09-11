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

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
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
constexpr uint32_t KERNEL_CACHE_CAPACITY = 5U;
constexpr uint32_t QUAD_VERTEX_COUNT = 6U;

struct alignas(16) GridPushConstants final {
    float world_size = 32.0F;
    float line_width = 0.03F;
    float pad0 = 0.0F;
    float pad1 = 0.0F;
    std::array<float, 4> line_color{ 0.16F, 0.16F, 0.18F, 1.0F };
};
static_assert(sizeof(GridPushConstants) == 32U);

struct alignas(16) PlayerPushConstants final {
    std::array<float, 2> origin{ 0.0F, 0.0F };
    float size = 2.0F;
    float pad0 = 0.0F;
    std::array<float, 4> color{ 1.0F, 1.0F, 1.0F, 1.0F };
};
static_assert(sizeof(PlayerPushConstants) == 32U);

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

} // namespace

struct VulkanRenderer::Impl final {
    using PresentationContext = core::graphics::vulkan::PresentationContext;
    using ResourceScope = PresentationContext::PresentationResourceScope;

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
        std::chrono::steady_clock::time_point const deadline
    )
    {
        std::optional<PresentationContext::Frame> frame;
        core::graphics::vulkan::PresentationAcquireResult const acquired = m_context->acquire(
            remaining(deadline),
            frame
        );
        if (acquired == core::graphics::vulkan::PresentationAcquireResult::NeedsRecreation) {
            recreate(m_context->info().extent, deadline);
            return false;
        }
        if (acquired != core::graphics::vulkan::PresentationAcquireResult::Ready || !frame.has_value()) {
            return false;
        }

        std::chrono::steady_clock::time_point const started_at = std::chrono::steady_clock::now();
        m_players = players;
        m_image_view = frame->imageView();
        frame->record(&Impl::recordFrame, this);
        m_context->complete(*frame);
        m_cpu_frame_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at
        );
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
            .cpu_frame_duration = m_cpu_frame_duration,
            .gpu_frame_duration = std::nullopt,
            .submitted_frame_count = m_submitted_frame_count,
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

    void record(VkCommandBuffer const command) const
    {
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = m_image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color.float32[0] = 0.06F;
        color_attachment.clearValue.color.float32[1] = 0.06F;
        color_attachment.clearValue.color.float32[2] = 0.08F;
        color_attachment.clearValue.color.float32[3] = 1.0F;
        VkExtent2D const extent = m_resources->extent();
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1U;
        rendering.colorAttachmentCount = 1U;
        rendering.pColorAttachments = &color_attachment;
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

        GridPushConstants const grid_push{ };
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grid_pipeline);
        vkCmdPushConstants(
            command,
            m_grid_layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0U,
            sizeof(grid_push),
            &grid_push
        );
        vkCmdDraw(command, QUAD_VERTEX_COUNT, GRID_WORKGROUPS_X * GRID_WORKGROUPS_Y, 0U, 0U);

        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_player_pipeline);
        for (PlayerRenderData const& player : m_players) {
            PlayerPushConstants const push{
                .origin = { static_cast<float>(player.x), static_cast<float>(player.y) },
                .color = player.color,
            };
            vkCmdPushConstants(
                command,
                m_player_layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0U,
                sizeof(push),
                &push
            );
            vkCmdDraw(command, QUAD_VERTEX_COUNT, 1U, 0U, 0U);
        }
        m_end_rendering(command);
    }

    [[nodiscard]] VkPipelineLayout createLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0U;
        range.size = sizeof(GridPushConstants);
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
        m_begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(m_resources->device(), "vkCmdBeginRenderingKHR")
        );
        m_end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(m_resources->device(), "vkCmdEndRenderingKHR")
        );
        if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
            throw std::runtime_error("Vulkan presentation device does not expose dynamic rendering commands");
        }
        auto const grid = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("grid.vert.spv")
        );
        auto const player = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("player.vert.spv")
        );
        auto const fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("trivial.frag.spv")
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
        m_grid_layout = createLayout();
        m_player_layout = createLayout();
        m_kernel_cache.emplace(KERNEL_CACHE_CAPACITY);
        core::graphics::vulkan::VulkanDeviceReference const device = m_resources->deviceReference();
        m_grid_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_grid_program,
            {
                .layout = m_grid_layout,
                .color_format = m_resources->format(),
            }
        );
        m_player_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_player_program,
            {
                .layout = m_player_layout,
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
        if (m_kernel_cache.has_value()) {
            m_kernel_cache->invalidate(m_resources->deviceReference().identity());
            m_kernel_cache.reset();
        }
        if (m_grid_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_grid_layout, nullptr); }
        if (m_player_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_player_layout, nullptr); }
        m_grid_pipeline = VK_NULL_HANDLE;
        m_player_pipeline = VK_NULL_HANDLE;
        m_grid_layout = VK_NULL_HANDLE;
        m_player_layout = VK_NULL_HANDLE;
        m_grid_program.reset();
        m_player_program.reset();
        m_begin_rendering = nullptr;
        m_end_rendering = nullptr;
        m_resources.reset();
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
    VkImageView m_image_view = VK_NULL_HANDLE;
    std::optional<core::kernel::GraphicsProgram> m_grid_program;
    std::optional<core::kernel::GraphicsProgram> m_player_program;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR m_begin_rendering = nullptr;
    PFN_vkCmdEndRenderingKHR m_end_rendering = nullptr;
    FrameCaptureState m_capture_state = FrameCaptureState::Disabled;
    std::optional<RendererFrameCapture> m_last_capture;
    std::chrono::nanoseconds m_cpu_frame_duration{ 0 };
    uint64_t m_submitted_frame_count = 0U;
    bool m_capture_requested = false;
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
    return m_impl->render(players, deadline);
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

} // namespace client
