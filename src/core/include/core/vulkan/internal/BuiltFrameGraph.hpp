#pragma once

#include <core/vulkan/FrameGraph.hpp>

namespace core::vk {

class FrameGraph::BuiltFrameGraph final {
    struct FrameGraphBuilder;
public:
    struct BuiltPass final {
        FramePassId id;
        Attachments attachments;
        RelativeViewport viewport;
        RelativeScissor scissor;
        bool needs_render_scope;
    };

    struct BarrierSlot final {
        std::vector<ImageMemoryBarrier> barriers;
        std::vector<AttachmentViewId> view_ids;

        void apply(
            FrameContext& frame_ctx,
            AttachmentViewProvider const& provider,
            std::vector<RawImage>& scratch_images
        );
    };

    std::vector<BuiltPass> passes{ };
    std::vector<BarrierSlot> barriers{ };
    AttachmentViewProvider provider{ };

    [[nodiscard]]
    static std::unique_ptr<BuiltFrameGraph> create(
        std::span<FullPassOptions const> const passes,
        std::span<FrameResource const> const resources,
        std::span<AttachmentViewId const> const resources_view_ids,
        AttachmentViewProvider&& provider
    );

    void applyBarrierSlot(size_t const slot_idx, FrameContext& frame_ctx) {
        barriers[slot_idx].apply(frame_ctx, provider, m_scratch_slot_images);
    }
private:
    std::vector<RawImage> m_scratch_slot_images{ };
};

} // namespace core::vk
