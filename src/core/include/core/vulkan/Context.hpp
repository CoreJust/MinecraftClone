// Context.hpp
#pragma once

#include <core/vulkan/CommandPool.hpp>
#include <core/vulkan/ContextBuilder.hpp>
#include <core/vulkan/Fence.hpp>
#include <core/vulkan/Semaphore.hpp>

#include <functional>

namespace core {

enum class ReloadType {
    Swapchain,
    Device,
    Surface,
    Instance,

    Count,
};

enum class ReloadSource {
    User,
    Error,

    Count,
};

enum class ReloadAction {
    Destroy,
    Recreate,

    Count,
};

CORE_ENUM_FUNCTIONS(ReloadType);
CORE_ENUM_FUNCTIONS(ReloadSource);
CORE_ENUM_FUNCTIONS(ReloadAction);

struct FailedToAcquireNextImage final : public VulkanError {
    FailedToAcquireNextImage() : VulkanError("Failed to acquire next image") { }
};
struct FailedToAdvanceFrame final : public VulkanError {
    FailedToAdvanceFrame(std::string reason) : VulkanError(reason) { }
};

class VulkanContext final : public VulkanCaps {
    struct ContextReloadHelper;
public:
    static constexpr size_t MAX_FRAMES_IN_FLIGHT = 3;
public:
    explicit VulkanContext(VulkanContextBuilder builder, Window const* window = nullptr);
    ~VulkanContext();

    void onReload(std::function<void(ReloadType const, ReloadSource const, ReloadAction const)>&& callback);
    void reload(ReloadType const type);

    void rebuild(auto&& fn, ReloadType const type = ReloadType::Instance) {
        fn(m_builder);
        reload(type);
    }

    // Returns false if frame cannot be started
    // Throws FailedToAcquireNextImage
    bool beginFrame();
    // Throws FailedToAdvanceFrame
    void endFrame();

    [[nodiscard]]
    constexpr size_t frameIndex() const noexcept { return m_frame_index; }
    [[nodiscard]]
    constexpr size_t wrappedFrameIndex() const noexcept { return m_frame_index % MAX_FRAMES_IN_FLIGHT; }

    void waitIdle() {
        if (!m_device.isNull()) {
            m_device.waitIdle();
        }
    }

    [[nodiscard]]
    constexpr std::optional<uint32_t> queueFamily(QueueFamily const family) const noexcept {
        return m_physical_device.queueFamily(family);
    }
    [[nodiscard]]
    constexpr Queue queue(QueueFamily const family) const noexcept {
        return m_device.queue(family);
    }

    [[nodiscard]]
    Image const& swapchainImage() const;
    [[nodiscard]]
    ImageView const& swapchainImageView() const;

    [[nodiscard]]
    Semaphore& imageAvailableSemaphore() noexcept {
        return m_image_available[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    Semaphore const& imageAvailableSemaphore() const noexcept {
        return m_image_available[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    Semaphore& renderFinishedSemaphore() noexcept {
        return m_render_finished[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    Semaphore const& renderFinishedSemaphore() const noexcept {
        return m_render_finished[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    Fence& inFlightFence() noexcept {
        return m_in_flight[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    Fence const& inFlightFence() const noexcept {
        return m_in_flight[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    RawCommandBuffer& commandBuffer() noexcept {
        return m_command_buffers[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }
    [[nodiscard]]
    RawCommandBuffer const& commandBuffer() const noexcept {
        return m_command_buffers[m_frame_index % MAX_FRAMES_IN_FLIGHT];
    }

    [[nodiscard]]
    constexpr Window const* window() const noexcept { return m_window; }
    [[nodiscard]]
    constexpr Instance& instance() noexcept { return m_instance; }
    [[nodiscard]]
    constexpr Instance const& instance() const noexcept { return m_instance; }
    [[nodiscard]]
    constexpr PhysicalDevice& physicalDevice() noexcept { return m_physical_device; }
    [[nodiscard]]
    constexpr PhysicalDevice const& physicalDevice() const noexcept { return m_physical_device; }
    [[nodiscard]]
    constexpr Surface& surface() noexcept { return m_surface; }
    [[nodiscard]]
    constexpr Surface const& surface() const noexcept { return m_surface; }
    [[nodiscard]]
    constexpr Device& device() noexcept { return m_device; }
    [[nodiscard]]
    constexpr Device const& device() const noexcept { return m_device; }
    [[nodiscard]]
    constexpr Swapchain& swapchain() noexcept { return m_swapchain; }
    [[nodiscard]]
    constexpr Swapchain const& swapchain() const noexcept { return m_swapchain; }
private:
    void createSyncObjects();
    void createCommandObjects();

    void reloadImpl(ReloadType const type, ReloadSource const source);
private:
    VulkanContextBuilder m_builder;
    Window const* m_window = nullptr;

    Instance m_instance;
Surface m_surface;
    PhysicalDevice m_physical_device;
    Device m_device;
    Swapchain m_swapchain;

    std::function<void(ReloadType const, ReloadSource const, ReloadAction const)> m_reload_callback;

    std::vector<Semaphore> m_image_available;
    std::vector<Semaphore> m_render_finished;
    std::vector<Fence> m_in_flight;
    core::CommandPool m_command_pool;
    core::CommandBuffers m_command_buffers;

    size_t m_frame_index = 0;
    uint32_t m_acquired_next_image_index = 0;
};

} // namespace core
