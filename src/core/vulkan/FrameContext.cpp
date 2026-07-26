#include <core/vulkan/FrameContext.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Context.hpp>

#include <volk.h>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::FrameContextErrorKind);

namespace core::vk {

RenderScope::~RenderScope() {
    m_p_ctx.endRenderScope(m_cmd);
}

FrameContext::~FrameContext() {
    m_p_ctx.endFrame();
}

void FrameContext::setImageBarriers(
    std::span<ImageMemoryBarrier const> const barriers,
    std::span<RawImage const> const images
) {
    ASSERT(barriers.size() == images.size());
    if (barriers.size() == 0) {
        return;
    }

    if (m_p_ctx.has(VulkanFeature::Synchronization2)) {
        std::vector<VkImageMemoryBarrier2> vk_barriers;
        vk_barriers.reserve(barriers.size());
        for (size_t i = 0; i < barriers.size(); ++i) {
            ImageMemoryBarrier const& b = barriers[i];
            RawImage image = images[i];
            vk_barriers.push_back(VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = static_cast<VkPipelineStageFlags2>(b.src_stages.value),
                .srcAccessMask = static_cast<VkAccessFlags2>(b.src_access.value),
                .dstStageMask = static_cast<VkPipelineStageFlags2>(b.dst_stages.value),
                .dstAccessMask = static_cast<VkAccessFlags2>(b.dst_access.value),
                .oldLayout = toVk<VkImageLayout>(b.old_layout),
                .newLayout = toVk<VkImageLayout>(b.new_layout),
                .image = image.handle(),
                .subresourceRange = VkImageSubresourceRange{
                    .aspectMask = b.aspect.value,
                    .baseMipLevel = b.base_mip_level,
                    .levelCount = b.level_count,
                    .baseArrayLayer = b.base_array_layer,
                    .layerCount = b.layer_count,
                },
            });
        }
        VkDependencyInfo dep_info {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = static_cast<uint32_t>(vk_barriers.size()),
            .pImageMemoryBarriers = vk_barriers.data(),
        };
        vkCmdPipelineBarrier2(m_command_buffer.handle(), &dep_info);
    } else {
        std::vector<VkImageMemoryBarrier> vk_barriers;
        vk_barriers.reserve(barriers.size());
        for (size_t i = 0; i < barriers.size(); ++i) {
            ImageMemoryBarrier const& b = barriers[i];
            RawImage image = images[i];
            if (b.src_stages.value > UINT32_MAX) {
                throw FrameContextError{
                    FrameContextErrorKind::UnsupportedMemoryBarrierStagesValue,
                    "Source pipeline stages contain values that require Synchronization2 feature to be enabled",
                };
            }
            if (b.dst_stages.value > UINT32_MAX) {
                throw FrameContextError{
                    FrameContextErrorKind::UnsupportedMemoryBarrierStagesValue,
                    "Destination pipeline stages contain values that require Synchronization2 feature to be enabled",
                };
            }
            if (b.src_access.value > UINT32_MAX) {
                throw FrameContextError{
                    FrameContextErrorKind::UnsupportedMemoryBarrierAccessValue,
                    "Source access flags contain values that require Synchronization2 feature to be enabled",
                };
            }
            if (b.dst_access.value > UINT32_MAX) {
                throw FrameContextError{
                    FrameContextErrorKind::UnsupportedMemoryBarrierAccessValue,
                    "Destination access flags contain values that require Synchronization2 feature to be enabled",
                };
            }

            vk_barriers.push_back(VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = static_cast<VkAccessFlags>(b.src_access.value),
                .dstAccessMask = static_cast<VkAccessFlags>(b.dst_access.value),
                .oldLayout = toVk<VkImageLayout>(b.old_layout),
                .newLayout = toVk<VkImageLayout>(b.new_layout),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image.handle(),
                .subresourceRange = VkImageSubresourceRange{
                    .aspectMask = b.aspect.value,
                    .baseMipLevel = b.base_mip_level,
                    .levelCount = b.level_count,
                    .baseArrayLayer = b.base_array_layer,
                    .layerCount = b.layer_count,
                },
            });
        }
        VkPipelineStageFlags src_stage = 0;
        VkPipelineStageFlags dst_stage = 0;
        for (auto& b : barriers) {
            src_stage |= static_cast<VkPipelineStageFlags>(b.src_stages.value);
            dst_stage |= static_cast<VkPipelineStageFlags>(b.dst_stages.value);
        }
        vkCmdPipelineBarrier(
            m_command_buffer.handle(),
            src_stage,
            dst_stage,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(vk_barriers.size()),
            vk_barriers.data()
        );
    }
}

void FrameContext::setImageBarrier(ImageMemoryBarrier const barrier) {
    setImageBarrier(barrier, m_p_ctx.swapchainImage().raw());
}

RenderScope FrameContext::acquireRenderScope(
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider,
    RelativeViewport viewport,
    RelativeScissor scissor
) {
    m_p_ctx.beginRenderScope(m_command_buffer, attachments, view_provider, viewport, scissor);
    return RenderScope{
        m_p_ctx,
        m_command_buffer,
    };
}

} // namespace core::vk
