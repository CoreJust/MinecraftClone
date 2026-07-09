#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>
#include <core/vulkan/CommandBuffer.hpp>
#include <core/vulkan/Fence.hpp>
#include <core/vulkan/Semaphore.hpp>

namespace core::vk {

class VulkanContext;

class FrameContext final : NonCopyable, NonMovable {
public:
    FrameContext(
        VulkanContext& p_ctx,
        RawSemaphore image_available,
        RawSemaphore render_finished,
        RawFence in_flight,
        RawCommandBuffer command_buffer,
        size_t frame_index,
        uint32_t acquired_next_image_index)
        : m_p_ctx(p_ctx)
        , m_image_available(image_available)
        , m_render_finished(render_finished)
        , m_in_flight(in_flight)
        , m_command_buffer(command_buffer)
        , m_frame_index(frame_index)
        , m_acquired_next_image_index(acquired_next_image_index)
    { }

    ~FrameContext();

    [[nodiscard]]
    VulkanContext& ctx() noexcept {
        return m_p_ctx;
    }
    [[nodiscard]]
    VulkanContext const& ctx() const noexcept {
        return m_p_ctx;
    }
    [[nodiscard]]
    RawSemaphore& imageAvailableSemaphore() noexcept {
        return m_image_available;
    }
    [[nodiscard]]
    RawSemaphore const& imageAvailableSemaphore() const noexcept {
        return m_image_available;
    }
    [[nodiscard]]
    RawSemaphore& renderFinishedSemaphore() noexcept {
        return m_render_finished;
    }
    [[nodiscard]]
    RawSemaphore const& renderFinishedSemaphore() const noexcept {
        return m_render_finished;
    }
    [[nodiscard]]
    RawFence& inFlightFence() noexcept {
        return m_in_flight;
    }
    [[nodiscard]]
    RawFence const& inFlightFence() const noexcept {
        return m_in_flight;
    }
    [[nodiscard]]
    RawCommandBuffer& commandBuffer() noexcept {
        return m_command_buffer;
    }
    [[nodiscard]]
    RawCommandBuffer const& commandBuffer() const noexcept {
        return m_command_buffer;
    }
private:
    VulkanContext& m_p_ctx;

    RawSemaphore m_image_available;
    RawSemaphore m_render_finished;
    RawFence m_in_flight;
    RawCommandBuffer m_command_buffer;

    size_t m_frame_index;
    uint32_t m_acquired_next_image_index;
};

} // namespace core::vk
