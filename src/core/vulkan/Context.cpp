// Context.cpp
#include <core/vulkan/Context.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/ErrorCallbacks.hpp>

#include <volk.h>

namespace core {

CORE_ENUM_FUNCTIONS_IMPL(ReloadType);
CORE_ENUM_FUNCTIONS_IMPL(ReloadSource);
CORE_ENUM_FUNCTIONS_IMPL(ReloadAction);

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
    core::setOutOfDateKHRCallback(ContextReloadHelper{ *this, ReloadType::Swapchain });
    core::setSuboptimalKHRCallback(ContextReloadHelper{ *this, ReloadType::Swapchain });
    core::setDeviceLostCallback(ContextReloadHelper{ *this, ReloadType::Device });
    core::setSurfaceLostCallback(ContextReloadHelper{ *this, ReloadType::Surface });
    core::setOutOfHostMemoryCallback(ContextReloadHelper{ *this, ReloadType::Instance });
    core::setOutOfDeviceMemoryCallback(ContextReloadHelper{ *this, ReloadType::Instance });
}

VulkanContext::~VulkanContext() {
    CORE_DEBUG("Destroying VulkanContext...");
    core::setOutOfDateKHRCallback(nullptr);
    core::setSuboptimalKHRCallback(nullptr);
    core::setDeviceLostCallback(nullptr);
    core::setSurfaceLostCallback(nullptr);
    core::setOutOfHostMemoryCallback(nullptr);
    core::setOutOfDeviceMemoryCallback(nullptr);
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

void VulkanContext::reloadImpl(ReloadType const type, ReloadSource const source) {
    if (!m_device.isNull()) {
        m_device.waitIdle();
    }
    if (m_reload_callback) {
        m_reload_callback(type, source, ReloadAction::Destroy);
    }
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
    m_command_pool = CommandPool(
        m_device,
        *queueFamily(core::QueueFamily::Graphics),
        core::CommandPoolFlags::of(core::CommandPoolFlag::ResetCommandBuffer)
    );
    m_command_buffers = m_command_pool.allocateBuffers(MAX_FRAMES_IN_FLIGHT);
}

bool VulkanContext::beginFrame() {
    if (!inFlightFence().wait()) {
        return false;
    }
    if (!VK_CHECK(vkAcquireNextImageKHR(
        m_device.handle(),
        m_swapchain.handle(),
        std::numeric_limits<uint64_t>::max(),
        imageAvailableSemaphore().handle(),
        VK_NULL_HANDLE,
        &m_acquired_next_image_index
    ))) {
        throw FailedToAcquireNextImage{ };
    }
    inFlightFence().reset();
    commandBuffer().reset();
    commandBuffer().begin();
    return true;
}

void VulkanContext::endFrame() {
    commandBuffer().end();

    VkPipelineStageFlags const waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo const submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = imageAvailableSemaphore().handlePtr(),
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = commandBuffer().handlePtr(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = renderFinishedSemaphore().handlePtr(),
    };
    if (!VK_CHECK(vkQueueSubmit(
        queue(core::QueueFamily::Graphics).handle(),
        1,
        &submit,
        inFlightFence().handle()
    ))) {
        throw FailedToAdvanceFrame{ "Failed to submit the current command buffer to graphics queue" };
    }
    
    VkPresentInfoKHR const present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = renderFinishedSemaphore().handlePtr(),
        .swapchainCount = 1,
        .pSwapchains = m_swapchain.handlePtr(),
        .pImageIndices = &m_acquired_next_image_index,
    };

    if (!VK_CHECK(vkQueuePresentKHR(queue(core::QueueFamily::Present).handle(), &present))) {
        throw FailedToAdvanceFrame{ "Failed to present the current frame" };
    }
    ++m_frame_index;
}

Image const& VulkanContext::swapchainImage() const {
    ASSERT(m_acquired_next_image_index < m_swapchain.images().size());
    return m_swapchain.images()[m_acquired_next_image_index];
}

ImageView const& VulkanContext::swapchainImageView() const {
    ASSERT(m_acquired_next_image_index < m_swapchain.imageViews().size());
    return m_swapchain.imageViews()[m_acquired_next_image_index];
}

} // namespace core
