// RenderPassCache.cpp
#include <core/vulkan/internal/RenderPassCache.hpp>

#include <core/common/Assert.hpp>
#include <core/common/HashCombine.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/ErrorCallbacks.hpp>

namespace core::vk {
namespace {

[[nodiscard]]
constexpr VkExtent2D extentFromAttachment(AttachmentView const& view) noexcept {
    return VkExtent2D{
        .width = view.image.width(),
        .height = view.image.height(),
    };
}

} // namespace

VkExtent2D resolveExtent(
    Attachments const& attachments,
    AttachmentViewProvider const& provider
) {
    if (attachments.colors.size() > 0) {
        return extentFromAttachment(provider[attachments.colors[0].view_id]);
    } else if (attachments.depth_stencil.has_value()) {
        return extentFromAttachment(provider[attachments.depth_stencil->view_id]);
    } else if (attachments.inputs.size() > 0) {
        return extentFromAttachment(provider[attachments.inputs[0].view_id]);
    } else if (attachments.color_resolves.size() > 0) {
        return extentFromAttachment(provider[attachments.color_resolves[0].view_id]);
    } else if (attachments.depth_stencil_resolve.has_value()) {
        return extentFromAttachment(provider[attachments.depth_stencil_resolve->view_id]);
    } else if (attachments.preserve.size() > 0) {
        return extentFromAttachment(provider[attachments.preserve[0].view_id]);
    } else {
        throw VulkanContextError{
            VulkanContextError::UnsupportedAttachmentConfiguration,
            "Render scope requires at least one attachment",
        };
    }
}

VulkanContext::RenderPassCache::RenderPassCache(Device const& device)
    : m_device(device)
{ }

VulkanContext::RenderPassCache::~RenderPassCache() {
    clear();
}

void VulkanContext::RenderPassCache::clear() noexcept {
    for (std::pair<RenderPassKey const, VkRenderPass>& entry : m_render_passes) {
        if (entry.second != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device.handle(), entry.second, nullptr);
        }
    }
    for (std::pair<FramebufferKey const, VkFramebuffer>& entry : m_framebuffers) {
        if (entry.second != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.handle(), entry.second, nullptr);
        }
    }

    m_render_passes.clear();
    m_framebuffers.clear();
}

size_t VulkanContext::RenderPassCache::RenderPassKeyHash::operator()(RenderPassKey const& key) const noexcept {
    HashCombiner combiner{ };
    for (ColorKey const& v : key.colors) {
        combiner.consume(v.format, v.load_op, v.store_op);
    }
    if (key.depth_stencil.has_value()) {
        DepthStencilKey const& v = key.depth_stencil.value();
        combiner.consume(v.format, v.depth_load_op, v.depth_store_op, v.stencil_load_op, v.stencil_store_op);
    }
    for (InputKey const& v : key.inputs) {
        combiner.consume(v.format);
    }
    for (std::optional<ResolveKey> const& v : key.color_resolves) {
        combiner.consume(v.has_value());
        if (v.has_value()) {
            combiner.consume(v->format);
        }
    }
    if (key.depth_stencil_resolve.has_value()) {
        DepthStencilResolveKey const& v = key.depth_stencil_resolve.value();
        combiner.consume(v.format, v.resolve_depth, v.resolve_stencil);
    }
    for (PreserveKey const& v : key.preserve) {
        combiner.consume(v.format);
    }

    return combiner.hash();
}

size_t VulkanContext::RenderPassCache::FramebufferKeyHash::operator()(FramebufferKey const& key) const noexcept {
    HashCombiner combiner{ };
    combiner.consume(key.render_pass, key.extent.width, key.extent.height);
    for (VkImageView const view : key.attachments) {
        combiner.consume(view);
    }

    return combiner.hash();
}

VulkanContext::RenderPassCache::RenderPassKey VulkanContext::RenderPassCache::makeRenderPassKey(
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider
) {
    RenderPassKey key{ };

    key.colors.reserve(attachments.colors.size());
    for (ColorAttachment const& attachment : attachments.colors) {
        AttachmentView const view = view_provider[attachment.view_id];
        key.colors.push_back(ColorKey{
            .format = static_cast<VkFormat>(view.image.format()),
            .load_op = static_cast<VkAttachmentLoadOp>(attachmentLoadOpToVk(attachment.ops.load)),
            .store_op = static_cast<VkAttachmentStoreOp>(attachmentStoreOpToVk(attachment.ops.store)),
        });
    }

    if (attachments.depth_stencil.has_value()) {
        DepthStencilAttachment const attachment = attachments.depth_stencil.value();
        AttachmentView const view = view_provider[attachment.view_id];
        key.depth_stencil = DepthStencilKey{
            .format = static_cast<VkFormat>(view.image.format()),
            .depth_load_op = static_cast<VkAttachmentLoadOp>(attachmentLoadOpToVk(attachment.depth_ops.load)),
            .depth_store_op = static_cast<VkAttachmentStoreOp>(attachmentStoreOpToVk(attachment.depth_ops.store)),
            .stencil_load_op = static_cast<VkAttachmentLoadOp>(attachmentLoadOpToVk(attachment.stencil_ops.load)),
            .stencil_store_op = static_cast<VkAttachmentStoreOp>(attachmentStoreOpToVk(attachment.stencil_ops.store)),
        };
    }

    key.inputs.reserve(attachments.inputs.size());
    for (InputAttachment const& attachment : attachments.inputs) {
        AttachmentView const view = view_provider[attachment.view_id];
        key.inputs.push_back(InputKey{
            .format = static_cast<VkFormat>(view.image.format()),
        });
    }

    key.color_resolves.resize(attachments.colors.size());
    for (size_t i = 0; i < attachments.color_resolves.size() && i < attachments.colors.size(); ++i) {
        AttachmentView const view = view_provider[attachments.color_resolves[i].view_id];
        key.color_resolves[i] = ResolveKey{
            .format = static_cast<VkFormat>(view.image.format()),
        };
    }

    if (attachments.depth_stencil_resolve.has_value()) {
        DepthStencilResolveAttachment const attachment = attachments.depth_stencil_resolve.value();
        AttachmentView const view = view_provider[attachment.view_id];
        key.depth_stencil_resolve = DepthStencilResolveKey{
            .format = static_cast<VkFormat>(view.image.format()),
            .resolve_depth = attachment.resolve_depth,
            .resolve_stencil = attachment.resolve_stencil,
        };
    }

    key.preserve.reserve(attachments.preserve.size());
    for (PreserveAttachment const& attachment : attachments.preserve) {
        AttachmentView const view = view_provider[attachment.view_id];
        key.preserve.push_back(PreserveKey{
            .format = static_cast<VkFormat>(view.image.format()),
        });
    }

    return key;
}

VulkanContext::RenderPassCache::FramebufferKey VulkanContext::RenderPassCache::makeFramebufferKey(
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider,
    VkRenderPass const render_pass,
    VkExtent2D const extent
) {
    FramebufferKey key{
        .render_pass = render_pass,
        .extent = extent,
    };

    key.attachments.reserve(0
        + attachments.colors.size()
        + attachments.depth_stencil.has_value()
        + attachments.inputs.size()
        + attachments.color_resolves.size()
        + attachments.depth_stencil_resolve.has_value()
        + attachments.preserve.size()
    );

    for (ColorAttachment const& attachment : attachments.colors) {
        key.attachments.push_back(view_provider[attachment.view_id].image_view.handle());
    }
    if (attachments.depth_stencil.has_value()) {
        key.attachments.push_back(view_provider[attachments.depth_stencil->view_id].image_view.handle());
    }
    for (InputAttachment const& attachment : attachments.inputs) {
        key.attachments.push_back(view_provider[attachment.view_id].image_view.handle());
    }
    for (ColorResolveAttachment const& attachment : attachments.color_resolves) {
        key.attachments.push_back(view_provider[attachment.view_id].image_view.handle());
    }
    if (attachments.depth_stencil_resolve.has_value()) {
        key.attachments.push_back(view_provider[attachments.depth_stencil_resolve->view_id].image_view.handle());
    }
    for (PreserveAttachment const& attachment : attachments.preserve) {
        key.attachments.push_back(view_provider[attachment.view_id].image_view.handle());
    }

    return key;
}

VkRenderPass VulkanContext::RenderPassCache::createRenderPass(
    VulkanContext const& ctx,
    RenderPassKey const& key
) const {
    if (key.depth_stencil_resolve.has_value()) {
        if (!ctx.hasExtensionOrPromoted(VulkanExtension::CreateRenderPass2)) {
            throw VulkanContextError{
                VulkanContextErrorKind::UnsupportedAttachmentConfiguration,
                "Depth/stencil resolve requires render pass 2 support",
            };
        }
        return createRenderPass2(key);
    }

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> input_refs;
    std::vector<VkAttachmentReference> color_refs;
    std::vector<VkAttachmentReference> resolve_refs;
    std::vector<uint32_t> preserve_indices;

    attachments.reserve(0
        + key.colors.size()
        + key.depth_stencil.has_value()
        + key.inputs.size()
        + key.color_resolves.size()
        + key.preserve.size()
    );
    input_refs.reserve(key.inputs.size());
    color_refs.reserve(key.colors.size());
    resolve_refs.reserve(key.colors.size());
    preserve_indices.reserve(key.preserve.size());

    for (ColorKey const& color_key : key.colors) {
        attachments.push_back(VkAttachmentDescription{
            .format = color_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = color_key.load_op,
            .storeOp = color_key.store_op,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        });
        color_refs.push_back(VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1U),
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        });
        resolve_refs.push_back(VkAttachmentReference{
            .attachment = VK_ATTACHMENT_UNUSED,
            .layout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
    }

    VkAttachmentReference depth_ref{
        .attachment = VK_ATTACHMENT_UNUSED,
        .layout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (key.depth_stencil.has_value()) {
        DepthStencilKey const& depth_key = key.depth_stencil.value();
        attachments.push_back(VkAttachmentDescription{
            .format = depth_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = depth_key.depth_load_op,
            .storeOp = depth_key.depth_store_op,
            .stencilLoadOp = depth_key.stencil_load_op,
            .stencilStoreOp = depth_key.stencil_store_op,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        });
        depth_ref = VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1U),
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
    }

    for (InputKey const& input_key : key.inputs) {
        attachments.push_back(VkAttachmentDescription{
            .format = input_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        input_refs.push_back(VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1U),
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
    }

    for (std::optional<ResolveKey> const& resolve_key : key.color_resolves) {
        if (!resolve_key.has_value()) {
            continue;
        }
        attachments.push_back(VkAttachmentDescription{
            .format = resolve_key->format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        });
    }

    for (PreserveKey const& preserve_key : key.preserve) {
        attachments.push_back(VkAttachmentDescription{
            .format = preserve_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        });
        preserve_indices.push_back(static_cast<uint32_t>(attachments.size() - 1U));
    }

    VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = static_cast<uint32_t>(input_refs.size()),
        .pInputAttachments = input_refs.data(),
        .colorAttachmentCount = static_cast<uint32_t>(color_refs.size()),
        .pColorAttachments = color_refs.data(),
        .pResolveAttachments = resolve_refs.data(),
        .pDepthStencilAttachment = key.depth_stencil.has_value() ? &depth_ref : nullptr,
        .preserveAttachmentCount = static_cast<uint32_t>(preserve_indices.size()),
        .pPreserveAttachments = preserve_indices.data(),
    };

    VkSubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = 0
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask = 0
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = 0
            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
    };

    VkRenderPassCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = static_cast<uint32_t>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateRenderPass(m_device.handle(), &info, nullptr, &render_pass))) {
        throw VulkanContextError{
            VulkanContextErrorKind::FailedToCreateRenderPass,
            "Failed to create render pass cache entry",
        };
    }

    return render_pass;
}

VkRenderPass VulkanContext::RenderPassCache::createRenderPass2(RenderPassKey const& key) const {
    std::vector<VkAttachmentDescription2> attachments;
    std::vector<VkAttachmentReference2> color_refs;
    std::vector<VkAttachmentReference2> input_refs;
    std::vector<VkAttachmentReference2> resolve_refs;
    std::vector<uint32_t> preserve_indices;

    attachments.reserve(1
        + key.colors.size()
        + key.depth_stencil.has_value()
        + key.inputs.size()
        + key.color_resolves.size()
        + key.preserve.size()
    );
    color_refs.reserve(key.colors.size());
    input_refs.reserve(key.inputs.size());
    resolve_refs.reserve(key.colors.size());
    preserve_indices.reserve(key.preserve.size());

    for (ColorKey const& color_key : key.colors) {
        attachments.push_back(VkAttachmentDescription2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = color_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = color_key.load_op,
            .storeOp = color_key.store_op,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        });
        color_refs.push_back(VkAttachmentReference2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = static_cast<uint32_t>(attachments.size() - 1U),
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        resolve_refs.push_back(VkAttachmentReference2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = VK_ATTACHMENT_UNUSED,
            .layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        });
    }

    std::optional<VkAttachmentReference2> depth_ref{ };
    if (key.depth_stencil.has_value()) {
        DepthStencilKey const& depth_key = key.depth_stencil.value();
        attachments.push_back(VkAttachmentDescription2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = depth_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = depth_key.depth_load_op,
            .storeOp = depth_key.depth_store_op,
            .stencilLoadOp = depth_key.stencil_load_op,
            .stencilStoreOp = depth_key.stencil_store_op,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        });
        depth_ref = VkAttachmentReference2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = static_cast<uint32_t>(attachments.size() - 1U),
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        };
    }

    for (InputKey const& input_key : key.inputs) {
        attachments.push_back(VkAttachmentDescription2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = input_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        input_refs.push_back(VkAttachmentReference2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = static_cast<uint32_t>(attachments.size() - 1U),
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        });
    }

    for (std::optional<ResolveKey> const& resolve_key : key.color_resolves) {
        if (!resolve_key.has_value()) {
            continue;
        }
        attachments.push_back(VkAttachmentDescription2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = resolve_key->format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        });
    }

    DepthStencilResolveKey const resolve_key = key.depth_stencil_resolve.value();
    attachments.push_back(VkAttachmentDescription2{
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
        .format = resolve_key.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    });
    VkAttachmentReference2 const depth_resolve_ref{
        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
        .attachment = static_cast<uint32_t>(attachments.size() - 1U),
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
    };

    for (PreserveKey const& preserve_key : key.preserve) {
        attachments.push_back(VkAttachmentDescription2{
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = preserve_key.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        });
        preserve_indices.push_back(static_cast<uint32_t>(attachments.size() - 1U));
    }

    VkSubpassDescriptionDepthStencilResolve depth_stencil_resolve{
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .pNext = nullptr,
        .depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
        .stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
        .pDepthStencilResolveAttachment = &depth_resolve_ref,
    };

    VkSubpassDescription2 subpass{
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
        .pNext = &depth_stencil_resolve,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .viewMask = 0,
        .inputAttachmentCount = static_cast<uint32_t>(input_refs.size()),
        .pInputAttachments = input_refs.data(),
        .colorAttachmentCount = static_cast<uint32_t>(color_refs.size()),
        .pColorAttachments = color_refs.data(),
        .pResolveAttachments = resolve_refs.data(),
        .pDepthStencilAttachment = depth_ref.has_value() ? &depth_ref.value() : nullptr,
        .preserveAttachmentCount = static_cast<uint32_t>(preserve_indices.size()),
        .pPreserveAttachments = preserve_indices.data(),
    };

    VkSubpassDependency2 dependency{
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
        .pNext = nullptr,
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = 0
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask = 0
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = 0
            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        .viewOffset = 0,
    };

    VkRenderPassCreateInfo2 info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = static_cast<uint32_t>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
        .correlatedViewMaskCount = 0,
        .pCorrelatedViewMasks = nullptr,
    };

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateRenderPass2(m_device.handle(), &info, nullptr, &render_pass))) {
        throw VulkanContextError{
            VulkanContextErrorKind::FailedToCreateRenderPass,
            "Failed to create render pass cache entry",
        };
    }

    return render_pass;
}

VkFramebuffer VulkanContext::RenderPassCache::createFramebuffer(FramebufferKey const& key) const {
    VkFramebufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = key.render_pass,
        .attachmentCount = static_cast<uint32_t>(key.attachments.size()),
        .pAttachments = key.attachments.data(),
        .width = key.extent.width,
        .height = key.extent.height,
        .layers = 1,
    };

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateFramebuffer(m_device.handle(), &info, nullptr, &framebuffer))) {
        throw VulkanContextError{
            VulkanContextErrorKind::FailedToCreateFramebuffer,
            "Failed to create framebuffer cache entry",
        };
    }

    return framebuffer;
}

VkRenderPass VulkanContext::RenderPassCache::getOrCreateRenderPass(
    VulkanContext const& ctx,
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider
) {
    RenderPassKey const key = makeRenderPassKey(attachments, view_provider);
    if (auto it = m_render_passes.find(key); it != m_render_passes.end()) {
        return it->second;
    }

    VkRenderPass const render_pass = createRenderPass(ctx, key);
    m_render_passes.emplace(key, render_pass);
    return render_pass;
}

VkFramebuffer VulkanContext::RenderPassCache::getOrCreateFramebuffer(
    Attachments const& attachments,
    AttachmentViewProvider const& view_provider,
    VkRenderPass const render_pass,
    VkExtent2D const extent
) {
    FramebufferKey const key = makeFramebufferKey(attachments, view_provider, render_pass, extent);
    if (auto it = m_framebuffers.find(key); it != m_framebuffers.end()) {
        return it->second;
    }

    VkFramebuffer const framebuffer = createFramebuffer(key);
    m_framebuffers.emplace(key, framebuffer);
    return framebuffer;
}

} // namespace core::vk
