#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/NonMovable.hpp>
#include <core/vulkan/Attachment.hpp>
#include <core/vulkan/CommandBuffer.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Fence.hpp>
#include <core/vulkan/FrameStructs.hpp>
#include <core/vulkan/Image.hpp>
#include <core/vulkan/ImageMemoryBarrier.hpp>
#include <core/vulkan/Semaphore.hpp>

CORE_VK_ERROR_WITH_KINDS(FrameContextError, VulkanRuntimeError,
    UnsupportedMemoryBarrierStagesValue,
    UnsupportedMemoryBarrierAccessValue);

namespace core::vk {

class VulkanContext;

class RenderScope final {
public:
    RenderScope(VulkanContext& ctx, RawCommandBuffer cmd) noexcept
        : m_p_ctx(ctx), m_cmd(cmd)
    { }
    ~RenderScope();
private:
    VulkanContext& m_p_ctx;
    RawCommandBuffer m_cmd;
};

class FrameContext : NonCopyable, NonMovable {
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
    
    void setImageBarriers(std::span<ImageMemoryBarrier const> const barriers, std::span<RawImage const> const images);
    void setImageBarrier(ImageMemoryBarrier const barrier);
    void setImageBarrier(ImageMemoryBarrier const barrier, RawImage const image) {
        setImageBarriers(unitSpan(barrier), unitSpan(image));
    }

    [[nodiscard]]
    RenderScope acquireRenderScope(
        Attachments const& attachments,
        AttachmentViewProvider const& view_provider,
        RelativeViewport viewport = { },
        RelativeScissor scissor = { }
    );

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
