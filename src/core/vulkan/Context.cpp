#include <core/vulkan/Context.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/ErrorCallbacks.hpp>
#include <core/vulkan/internal/RenderPassCache.hpp>

#include <volk.h>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::VulkanContextErrorKind);
CORE_ENUM_FUNCTIONS_IMPL(::core::vk::ReloadType);
CORE_ENUM_FUNCTIONS_IMPL(::core::vk::ReloadSource);
CORE_ENUM_FUNCTIONS_IMPL(::core::vk::ReloadAction);

namespace core::vk {
namespace {

[[nodiscard]]
VkRenderingAttachmentInfo makeColorRenderingAttachment(
    ColorAttachment const& attachment,
    AttachmentView const view,
    std::optional<ColorResolveAttachment> const resolve_attachment
) {
    VkRenderingAttachmentInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view.image_view.handle(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = toVk<VkAttachmentLoadOp>(attachment.ops.load),
        .storeOp = toVk<VkAttachmentStoreOp>(attachment.ops.store),
        .clearValue = attachment.clear_color.has_value()
            ? makeColorClearValue(*attachment.clear_color)
            : VkClearValue{ },
    };

    if (resolve_attachment.has_value()) {
        info.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        info.resolveImageView = view.image_view.handle();
        info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    return info;
}

void applyViewportAndScissor(
    RawCommandBuffer const command_buffer,
    RelativeViewport const viewport,
    RelativeScissor const scissor,
    VkExtent2D const extent
) {
    VkViewport const vk_viewport{
        .x = viewport.x * static_cast<float>(extent.width),
        .y = viewport.y * static_cast<float>(extent.height),
        .width = viewport.w * static_cast<float>(extent.width),
        .height = viewport.h * static_cast<float>(extent.height),
        .minDepth = viewport.depth_min,
        .maxDepth = viewport.depth_max,
    };
    vkCmdSetViewport(command_buffer.handle(), 0, 1, &vk_viewport);

    VkRect2D const vk_scissor{
        .offset = {
            .x = static_cast<int32_t>(scissor.x * static_cast<float>(extent.width)),
            .y = static_cast<int32_t>(scissor.y * static_cast<float>(extent.height)),
        },
        .extent = {
            .width = static_cast<uint32_t>(scissor.w * static_cast<float>(extent.width)),
            .height = static_cast<uint32_t>(scissor.h * static_cast<float>(extent.height)),
        },
    };
    vkCmdSetScissor(command_buffer.handle(), 0, 1, &vk_scissor);
}

} // namespace

struct VulkanContext::ContextReloadHelper final {
    VulkanContext& self;
    ReloadType type;

    bool operator()() {
        self.reloadImpl(type, ReloadSource::Error);
        return true;
    }
};

VulkanContext::VulkanContext(VulkanContextBuilder builder, Window const* window)
    : m_builder(std::move(builder))
    , m_window(window)
    , m_instance(m_builder.buildInstance(*this))
    , m_surface(
        window
            ? Surface(m_instance, *window)
            : Surface()
    )
    , m_physical_device(m_builder.selectPhysicalDevice(
        *this,
        m_instance,
        m_surface.isNull() ? nullptr : &m_surface
    ))
    , m_device(m_builder.buildDevice(*this, m_physical_device))
    , m_swapchain(
        m_surface.isNull()
            ? Swapchain()
            : m_builder.buildSwapchain(*this, m_device, m_physical_device, m_surface)
    )
{
    if (!m_surface.isNull()) {
        createSyncObjects();
        createCommandObjects();
    }
    setOutOfDateKHRCallback(ContextReloadHelper{ *this, ReloadType::Swapchain });
    setSuboptimalKHRCallback(ContextReloadHelper{ *this, ReloadType::Swapchain });
    setDeviceLostCallback(ContextReloadHelper{ *this, ReloadType::Device });
    setSurfaceLostCallback(ContextReloadHelper{ *this, ReloadType::Surface });
    setOutOfHostMemoryCallback(ContextReloadHelper{ *this, ReloadType::Instance });
    setOutOfDeviceMemoryCallback(ContextReloadHelper{ *this, ReloadType::Instance });
}

VulkanContext::VulkanContext(VulkanContext&&) noexcept = default;

VulkanContext::~VulkanContext() {
    CORE_DEBUG("Destroying VulkanContext...");
    setOutOfDateKHRCallback(nullptr);
    setSuboptimalKHRCallback(nullptr);
    setDeviceLostCallback(nullptr);
    setSurfaceLostCallback(nullptr);
    setOutOfHostMemoryCallback(nullptr);
    setOutOfDeviceMemoryCallback(nullptr);
    if (!m_device.isNull()) {
        m_device.waitIdle();
    }
    CORE_DEBUG("VulkanContext destroyed");
}

void VulkanContext::onReload(std::function<void(ReloadType const, ReloadSource const, ReloadAction const)>&& callback) {
    m_reload_callback = std::move(callback);
}

void VulkanContext::reload(ReloadType const type) {
    reloadImpl(type, ReloadSource::User);
}

std::optional<FrameContext> VulkanContext::acquireFrame() {
    size_t const frame_idx = m_frame_index % MAX_FRAMES_IN_FLIGHT;
    if (!m_in_flight[frame_idx].wait()) {
        return std::nullopt;
    }
    if (!VK_CHECK(vkAcquireNextImageKHR(
        m_device.handle(),
        m_swapchain.handle(),
        std::numeric_limits<uint64_t>::max(),
        m_image_available[frame_idx].handle(),
        VK_NULL_HANDLE,
        &m_acquired_next_image_index
    ))) {
        throw VulkanContextError{ VulkanContextError::FailedToAcquireNextImage };
    }

    m_in_flight[frame_idx].reset();
    m_command_buffers[frame_idx].reset();
    m_command_buffers[frame_idx].begin();
    return std::make_optional<FrameContext>(
        *this,
        m_image_available[frame_idx],
        m_render_finished[frame_idx],
        m_in_flight[frame_idx],
        m_command_buffers[frame_idx],
        frame_idx,
        m_acquired_next_image_index
    );
}

void VulkanContext::endFrame() {
    size_t const frame_idx = m_frame_index % MAX_FRAMES_IN_FLIGHT;
    m_command_buffers[frame_idx].end();

    VkPipelineStageFlags const waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo const submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = m_image_available[frame_idx].handlePtr(),
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = m_command_buffers[frame_idx].handlePtr(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = m_render_finished[frame_idx].handlePtr(),
    };
    if (!VK_CHECK(vkQueueSubmit(
        queue(QueueFamily::Graphics).handle(),
        1,
        &submit,
        m_in_flight[frame_idx].handle()
    ))) {
        throw VulkanContextError{
            VulkanContextError::FailedToAdvanceFrame,
            "Failed to submit the current command buffer to graphics queue"
        };
    }
    
    VkPresentInfoKHR const present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = m_render_finished[frame_idx].handlePtr(),
        .swapchainCount = 1,
        .pSwapchains = m_swapchain.handlePtr(),
        .pImageIndices = &m_acquired_next_image_index,
    };

    if (!VK_CHECK(vkQueuePresentKHR(queue(QueueFamily::Present).handle(), &present))) {
        throw VulkanContextError{
            VulkanContextError::FailedToAdvanceFrame,
            "Failed to present the current frame"
        };
    }
    ++m_frame_index;
}

void VulkanContext::beginRenderScope(
    RawCommandBuffer cmd,
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider,
    RelativeViewport viewport,
    RelativeScissor scissor
) {
    ASSERT(!m_render_scope_open);

    if (attachments.color_resolves.size() > attachments.colors.size()) {
        throw VulkanContextError{
            VulkanContextErrorKind::UnsupportedAttachmentConfiguration,
            "More color resolve attachments than color attachments",
        };
    }

    VkExtent2D const extent = resolveExtent(attachments, view_provider);
    if (has(VulkanFeature::DynamicRendering)
        && attachments.inputs.size() == 0
        && attachments.preserve.size() == 0
        && !attachments.depth_stencil_resolve.has_value()
    ) {
        return beginDynamicRenderScope(cmd, attachments, view_provider, extent, viewport, scissor);
    }

    if (!m_render_pass_cache) {
        m_render_pass_cache = std::make_unique<RenderPassCache>(m_device);
    }

    VkRenderPass const render_pass = m_render_pass_cache->getOrCreateRenderPass(
        *this,
        attachments,
        view_provider
    );
    VkFramebuffer const framebuffer = m_render_pass_cache->getOrCreateFramebuffer(
        attachments,
        view_provider,
        render_pass,
        extent
    );

    VkClearValue clear_values[2]{ };
    uint32_t clear_value_count = 0;

    if (attachments.colors.size() > 0) {
        ColorAttachment const& attachment = attachments.colors[0];
        if (attachment.clear_color.has_value()) {
            clear_values[clear_value_count] = makeColorClearValue(*attachment.clear_color);
        }
        ++clear_value_count;
    }

    if (attachments.depth_stencil.has_value()) {
        DepthStencilAttachment const& attachment = attachments.depth_stencil.value();
        if (attachment.clear_depth.has_value()) {
            clear_values[clear_value_count].depthStencil.depth = attachment.clear_depth.value();
        }
        if (attachment.clear_stencil.has_value()) {
            clear_values[clear_value_count].depthStencil.stencil = attachment.clear_stencil.value();
        }
        ++clear_value_count;
    }

    VkRenderPassBeginInfo const begin_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea = VkRect2D{ .offset = { 0, 0 }, .extent = extent },
        .clearValueCount = clear_value_count,
        .pClearValues = clear_values,
    };

    vkCmdBeginRenderPass(cmd.handle(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    applyViewportAndScissor(cmd, viewport, scissor, extent);

    m_render_scope_is_dynamic = false;
    m_render_scope_open = true;
}

void VulkanContext::endRenderScope(RawCommandBuffer cmd) {
    ASSERT(m_render_scope_open);

    if (m_render_scope_is_dynamic) {
        vkCmdEndRendering(cmd.handle());
    } else {
        vkCmdEndRenderPass(cmd.handle());
    }
    m_render_scope_open = false;
    m_render_scope_is_dynamic = false;
}

Image const& VulkanContext::swapchainImage() const {
    ASSERT(m_acquired_next_image_index < m_swapchain.images().size());
    return m_swapchain.images()[m_acquired_next_image_index];
}

ImageView const& VulkanContext::swapchainImageView() const {
    ASSERT(m_acquired_next_image_index < m_swapchain.imageViews().size());
    return m_swapchain.imageViews()[m_acquired_next_image_index];
}

void VulkanContext::beginDynamicRenderScope(
    RawCommandBuffer cmd,
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider,
    VkExtent2D const extent,
    RelativeViewport viewport,
    RelativeScissor scissor
) {
    std::vector<VkRenderingAttachmentInfo> color_attachments;
    color_attachments.reserve(attachments.colors.size());

    for (size_t i = 0; i < attachments.colors.size(); ++i) {
        ColorAttachment const& attachment = attachments.colors[i];
        AttachmentView const view = view_provider[attachment.view_id];

        std::optional<ColorResolveAttachment> resolve_attachment{ };
        if (i < attachments.color_resolves.size()) {
            resolve_attachment = attachments.color_resolves[i];
        }

        color_attachments.push_back(makeColorRenderingAttachment(
            attachment,
            view,
            resolve_attachment
        ));
    }

    std::optional<VkRenderingAttachmentInfo> depth_attachment{ };
    std::optional<VkRenderingAttachmentInfo> stencil_attachment{ };

    if (attachments.depth_stencil.has_value()) {
        DepthStencilAttachment const attachment = attachments.depth_stencil.value();
        AttachmentView const view = view_provider[attachment.view_id];

        depth_attachment = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view.image_view.handle(),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = toVk<VkAttachmentLoadOp>(attachment.depth_ops.load),
            .storeOp = toVk<VkAttachmentStoreOp>(attachment.depth_ops.store),
            .clearValue = attachment.clear_depth.has_value()
                ? makeDepthClearValue(*attachment.clear_depth)
                : VkClearValue{ },
        };
        stencil_attachment = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view.image_view.handle(),
            .imageLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = toVk<VkAttachmentLoadOp>(attachment.stencil_ops.load),
            .storeOp = toVk<VkAttachmentStoreOp>(attachment.stencil_ops.store),
            .clearValue = VkClearValue{ .depthStencil = {
                .stencil = attachment.clear_stencil.value_or(0),
            }},
        };
    }

    VkRenderingInfo const rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = VkRect2D{ .offset = { 0, 0 }, .extent = extent },
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = static_cast<uint32_t>(color_attachments.size()),
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = depth_attachment.has_value() ? &depth_attachment.value() : nullptr,
        .pStencilAttachment = stencil_attachment.has_value() ? &stencil_attachment.value() : nullptr,
    };

    vkCmdBeginRendering(cmd.handle(), &rendering_info);
    applyViewportAndScissor(cmd, viewport, scissor, extent);

    m_render_scope_is_dynamic = true;
    m_render_scope_open = true;
}

void VulkanContext::createSyncObjects() {
    m_image_available.reserve(MAX_FRAMES_IN_FLIGHT);
    m_render_finished.reserve(MAX_FRAMES_IN_FLIGHT);
    m_in_flight.reserve(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_image_available.emplace_back(m_device);
        m_render_finished.emplace_back(m_device);
        m_in_flight.emplace_back(m_device, FenceSignaled::Yes);
    }
}

void VulkanContext::createCommandObjects() {
    m_command_pool = CommandPool{
        m_device,
        *queueFamily(QueueFamily::Graphics),
        CommandPoolFlag::ResetCommandBuffer,
    };
    m_command_buffers = m_command_pool.allocateBuffers(MAX_FRAMES_IN_FLIGHT);
}

void VulkanContext::reloadImpl(ReloadType const type, ReloadSource const source) {
    if (!m_device.isNull()) {
        m_device.waitIdle();
    }
    if (m_reload_callback) {
        m_reload_callback(type, source, ReloadAction::Destroy);
    }
    m_render_pass_cache.reset();
    m_swapchain = Swapchain{ };
    if (indexOf(type) >= indexOf(ReloadType::Device)) {
        m_command_buffers = CommandBuffers{ };
        m_command_pool = CommandPool( );
        m_image_available.clear();
        m_render_finished.clear();
        m_in_flight.clear();
        m_device = Device{ };
        m_physical_device = PhysicalDevice{ };
    }
    if (indexOf(type) >= indexOf(ReloadType::Surface)) {
        m_surface = Surface{ };
    }
    if (type == ReloadType::Instance) {
        m_instance = Instance{ };
    }
    switch (type) {
        case ReloadType::Instance:
            m_instance = m_builder.buildInstance(*this);
            [[fallthrough]];
        case ReloadType::Surface:
            if (m_window) {
                m_surface = Surface(m_instance, *m_window);
            }
            [[fallthrough]];
        case ReloadType::Device:
            m_physical_device = m_builder.selectPhysicalDevice(
                *this,
                m_instance,
                m_surface.isNull() ? nullptr : &m_surface
            );
            m_device = m_builder.buildDevice(*this, m_physical_device);
            if (!m_surface.isNull()) {
                createSyncObjects();
                createCommandObjects();
            }
            [[fallthrough]];
        case ReloadType::Swapchain:
            if (!m_surface.isNull()) {
                m_swapchain = m_builder.rebuildSwapchain(
                    *this,
                    m_device,
                    m_physical_device,
                    m_surface,
                    m_swapchain
                );
            }
            break;
    default: UNREACHABLE();
    }
    if (m_reload_callback) {
        m_reload_callback(type, source, ReloadAction::Recreate);
    }
}

} // namespace core::vk
