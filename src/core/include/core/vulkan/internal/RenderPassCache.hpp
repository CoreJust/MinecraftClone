#pragma once

#include <core/vulkan/Context.hpp>

#include <volk.h>

namespace core::vk {

[[nodiscard]]
constexpr VkClearValue makeColorClearValue(Color4 const& с) noexcept {
    return VkClearValue{
        .color = VkClearColorValue{{ с.r, с.g, с.b, с.a }},
    };
}

[[nodiscard]]
constexpr VkClearValue makeDepthClearValue(float const depth) noexcept {
    return VkClearValue{
        .depthStencil = VkClearDepthStencilValue{ .depth = depth, .stencil = 0 },
    };
}

[[nodiscard]]
VkExtent2D resolveExtent(
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider
);

struct VulkanContext::RenderPassCache final {
public:
    explicit RenderPassCache(Device const& device);
    ~RenderPassCache();

    void clear() noexcept;

    [[nodiscard]]
    VkRenderPass getOrCreateRenderPass(
        VulkanContext const& ctx,
        Attachments const& attachments,
        AttachmentViewProvider const& view_provider
    );

    [[nodiscard]]
    VkFramebuffer getOrCreateFramebuffer(
        Attachments const& attachments,
        AttachmentViewProvider const& view_provider,
        VkRenderPass render_pass,
        VkExtent2D extent
    );
private:
    struct ColorKey final {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;

        [[nodiscard]]
        bool operator==(ColorKey const&) const noexcept = default;
    };

    struct DepthStencilKey final {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkAttachmentLoadOp depth_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkAttachmentLoadOp stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;

        [[nodiscard]]
        bool operator==(DepthStencilKey const&) const noexcept = default;
    };

    struct InputKey final {
        VkFormat format = VK_FORMAT_UNDEFINED;

        [[nodiscard]]
        bool operator==(InputKey const&) const noexcept = default;
    };

    struct ResolveKey final {
        VkFormat format = VK_FORMAT_UNDEFINED;

        [[nodiscard]]
        bool operator==(ResolveKey const&) const noexcept = default;
    };

    struct DepthStencilResolveKey final {
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool resolve_depth = false;
        bool resolve_stencil = false;

        [[nodiscard]]
        bool operator==(DepthStencilResolveKey const&) const noexcept = default;
    };

    struct PreserveKey final {
        VkFormat format = VK_FORMAT_UNDEFINED;

        [[nodiscard]]
        bool operator==(PreserveKey const&) const noexcept = default;
    };

    struct RenderPassKey final {
        std::vector<ColorKey> colors{ };
        std::optional<DepthStencilKey> depth_stencil{ };
        std::vector<InputKey> inputs{ };
        std::vector<std::optional<ResolveKey>> color_resolves{ };
        std::optional<DepthStencilResolveKey> depth_stencil_resolve{ };
        std::vector<PreserveKey> preserve{ };

        [[nodiscard]]
        bool operator==(RenderPassKey const&) const noexcept = default;
    };

    struct FramebufferKey final {
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkExtent2D extent{ 0, 0 };
        std::vector<VkImageView> attachments{ };

        [[nodiscard]]
        bool operator==(FramebufferKey const& rhs) const noexcept {
            return true
                && render_pass == rhs.render_pass
                && extent.width == rhs.extent.width
                && extent.height == rhs.extent.height
                && attachments == rhs.attachments;
        }
    };

    struct RenderPassKeyHash final {
        [[nodiscard]]
        size_t operator()(RenderPassKey const& key) const noexcept;
    };

    struct FramebufferKeyHash final {
        [[nodiscard]]
        size_t operator()(FramebufferKey const& key) const noexcept;
    };
private:
    [[nodiscard]]
    static RenderPassKey makeRenderPassKey(
        Attachments const& attachments,
        AttachmentViewProvider const& view_provider
    );

    [[nodiscard]]
    static FramebufferKey makeFramebufferKey(
        Attachments const& attachments,
        AttachmentViewProvider const& view_provider,
        VkRenderPass render_pass,
        VkExtent2D extent
    );

    [[nodiscard]]
    VkRenderPass createRenderPass(VulkanContext const& ctx, RenderPassKey const& key) const;
    [[nodiscard]]
    VkRenderPass createRenderPass2(RenderPassKey const& key) const;
    [[nodiscard]]
    VkFramebuffer createFramebuffer(FramebufferKey const& key) const;
private:
    Device const& m_device;
    std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> m_render_passes;
    std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> m_framebuffers;
};

} // namespace core::vk
