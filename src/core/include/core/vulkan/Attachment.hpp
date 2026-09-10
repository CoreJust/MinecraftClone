#pragma once

#include <core/common/Color.hpp>
#include <core/common/TrivialPair.hpp>
#include <core/vulkan/Image.hpp>
#include <core/vulkan/ImageView.hpp>
#include <core/vulkan/enum/AttachmentOp.hpp>

namespace core::vk {

enum class AttachmentViewId : uint32_t { };

struct ColorAttachment final {
    AttachmentViewId view_id;
    AttachmentOps ops{ };
    std::optional<Color4> clear_color{ };

    [[nodiscard]]
    static constexpr ColorAttachment load(
        AttachmentViewId const view_id,
        AttachmentStoreOp const store_op = AttachmentStoreOp::Store
    ) noexcept {
        return {
            .view_id = view_id,
            .ops = { .store = store_op },
        };
    }

    [[nodiscard]]
    static constexpr ColorAttachment clear(
        AttachmentViewId const view_id,
        Color4 const clear,
        AttachmentStoreOp const store_op = AttachmentStoreOp::Store
    ) noexcept {
        return {
            .view_id = view_id,
            .ops = { .load = AttachmentLoadOp::Clear, .store = store_op },
            .clear_color = clear,
        };
    }
};

struct DepthStencilAttachment final {
    AttachmentViewId view_id;

    AttachmentOps depth_ops{ };
    AttachmentOps stencil_ops{ };

    std::optional<float> clear_depth{ };
    std::optional<uint32_t> clear_stencil{ };

    [[nodiscard]]
    static constexpr DepthStencilAttachment load(AttachmentViewId const view_id) noexcept {
        return { .view_id = view_id };
    }

    [[nodiscard]]
    static constexpr DepthStencilAttachment clearDepth(
        AttachmentViewId const view_id,
        float const depth
    ) noexcept {
        return {
            .view_id = view_id,
            .depth_ops = { .load = AttachmentLoadOp::Clear },
            .clear_depth = depth,
        };
    }

    [[nodiscard]]
    static constexpr DepthStencilAttachment clearStencil(
        AttachmentViewId const view_id,
        uint32_t const stencil
    ) noexcept {
        return {
            .view_id = view_id,
            .stencil_ops = { .load = AttachmentLoadOp::Clear },
            .clear_stencil = stencil,
        };
    }

    [[nodiscard]]
    static constexpr DepthStencilAttachment clearDepthStencil(
        AttachmentViewId const view_id,
        float const depth,
        uint32_t const stencil
    ) noexcept {
        return {
            .view_id = view_id,
            .depth_ops = { .load = AttachmentLoadOp::Clear },
            .stencil_ops = { .load = AttachmentLoadOp::Clear },
            .clear_depth = depth,
            .clear_stencil = stencil,
        };
    }
};

struct InputAttachment final {
    AttachmentViewId view_id;
};

struct ColorResolveAttachment final {
    AttachmentViewId view_id;
};

struct DepthStencilResolveAttachment final {
    AttachmentViewId view_id;
    bool resolve_depth = false;
    bool resolve_stencil = false;

    [[nodiscard]]
    static constexpr DepthStencilResolveAttachment resolveDepth(
        AttachmentViewId const view_id
    ) {
        return {
            .view_id = view_id,
            .resolve_depth = true,
        };
    }

    [[nodiscard]]
    static constexpr DepthStencilResolveAttachment resolveStencil(
        AttachmentViewId const view_id
    ) {
        return {
            .view_id = view_id,
            .resolve_stencil = true,
        };
    }

    [[nodiscard]]
    static constexpr DepthStencilResolveAttachment resolveDepthStencil(
        AttachmentViewId const view_id
    ) {
        return {
            .view_id = view_id,
            .resolve_depth = true,
            .resolve_stencil = true,
        };
    }
};

struct PreserveAttachment final {
    AttachmentViewId view_id;
};

struct Attachments final {
    std::vector<ColorAttachment> colors{ };
    std::optional<DepthStencilAttachment> depth_stencil{ };
    std::vector<InputAttachment> inputs{ };
    std::vector<ColorResolveAttachment> color_resolves{ };
    std::optional<DepthStencilResolveAttachment> depth_stencil_resolve{ };
    std::vector<PreserveAttachment> preserve{ };
};

struct AttachmentView final {
    RawImage const& image;
    RawImageView const& image_view;
};

class AttachmentViewProvider final {
public:
    AttachmentViewProvider(size_t const capacity = 0) { m_attachment_views.reserve(capacity); }

    [[nodiscard]]
    AttachmentViewId createId() noexcept {
        m_attachment_views.emplace_back();
        return static_cast<AttachmentViewId>(m_attachment_views.size() - 1);
    }

    AttachmentView operator[](AttachmentViewId const id) const;
    void bind(AttachmentViewId const id, AttachmentView const view);

    void unbindAll() noexcept;
private:
    std::vector<TrivialPair<RawImage const*, RawImageView const*>> m_attachment_views;
};

} // namespace core::vk
