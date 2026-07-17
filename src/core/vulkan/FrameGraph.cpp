#include <core/vulkan/FrameGraph.hpp>

#include <core/common/Assert.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/internal/BuiltFrameGraph.hpp>

CORE_ENUM_FUNCTIONS_IMPL(vk::FrameGraphErrorKind);

namespace core::vk {

FrameGraph::FrameGraph(VulkanContext&& ctx) : m_ctx(std::move(ctx)) { }
FrameGraph::FrameGraph(FrameGraph&&) noexcept = default;
FrameGraph::~FrameGraph() = default;

FrameResourceId FrameGraph::importSwapchain(std::optional<Color4> const clear_color) {
    m_resources.emplace_back(FrameResource{
        .clear_color = clear_color,
        .kind = FrameResource::Kind::Swapchain,
    });
    discard();
    return static_cast<FrameResourceId>(m_resources.size() - 1);
}

FramePassId FrameGraph::add(FramePassOptions&& options) {
    FramePassId const id = static_cast<FramePassId>(m_passes.size());
    if (options.name.empty()) {
        options.name = "pass_" + std::to_string(static_cast<uint32_t>(id));
    }
    m_passes.emplace_back(FullPassOptions{
        .options = std::move(options),
        .callback = FramePassBindCallback{},
        .persistent = PersistentFramePassBindCallback::No,
    });
    discard();
    return id;
}

void FrameGraph::bind(
    FramePassId const id,
    FramePassBindCallback&& cb,
    PersistentFramePassBindCallback const persistent
) {
    ASSERT(cb);
    size_t const index = static_cast<size_t>(id);
    if (index >= m_passes.size()) {
        throw FrameGraphError{
            FrameGraphError::InvalidPassId,
            "FrameGraph::bind: pass id is out of range",
        };
    }
    m_passes[index].callback   = std::move(cb);
    m_passes[index].persistent = persistent;
}

void FrameGraph::discard() {
    m_built_graph.reset();
}

void FrameGraph::build() {
    if (m_passes.empty()) {
        m_built_graph = std::make_unique<BuiltFrameGraph>();
        return;
    }

    size_t const pass_count = m_passes.size();
    size_t const res_count  = m_resources.size();
    for (FullPassOptions const& pass : m_passes) {
        for (FramePassId const dep_id : pass.options.dependencies) {
            if (static_cast<size_t>(dep_id) >= pass_count) {
                throw FrameGraphError{
                    FrameGraphError::InvalidPassId,
                    "dependency references an invalid pass",
                };
            }
        }
        for (FrameResourceId const res_id : pass.options.read_resources) {
            if (static_cast<size_t>(res_id) >= res_count) {
                throw FrameGraphError{
                    FrameGraphError::InvalidResourceId,
                    "read resource id out of range",
                };
            }
        }
        for (FrameResourceId const res_id : pass.options.written_resources) {
            if (static_cast<size_t>(res_id) >= res_count) {
                throw FrameGraphError{
                    FrameGraphError::InvalidResourceId,
                    "written resource id out of range",
                };
            }
        }
    }

    AttachmentViewProvider provider{ };
    m_resources_view_ids.clear();
    m_resources_view_ids.reserve(m_resources.size());
    for (FrameResource const& res : m_resources) {
        (void)res;
        m_resources_view_ids.emplace_back(provider.createId());
    }

    m_built_graph = BuiltFrameGraph::create(m_passes, m_resources, m_resources_view_ids, std::move(provider));
}

void FrameGraph::render() {
    if (m_ctx.window() != nullptr && m_ctx.window()->isFramebufferSizeZero()) {
        return;
    }
    if (!m_built_graph) {
        build();
    }

    auto maybe_frame = m_ctx.acquireFrame();
    if (!maybe_frame.has_value()) {
        CORE_WARN("Failed to acquire frame {}; skipping the frame", m_ctx.frameIndex());
        return;
    }

    FrameContext& frame_ctx = maybe_frame.value();
    RawCommandBuffer const cmd = frame_ctx.commandBuffer();
    AttachmentViewProvider& provider = m_built_graph->provider;

    for (size_t i = 0; i < m_resources.size(); ++i) {
        FrameResource const& res = m_resources[i];
        AttachmentViewId const view_id = m_resources_view_ids[i];
        switch (res.kind) {
            case FrameResource::Kind::Swapchain: {
                provider.bind(view_id, AttachmentView{
                    m_ctx.swapchainImage(),
                    m_ctx.swapchainImageView(),
                });
                break;
            }
        default:
            throw FrameGraphError{
                FrameGraphError::UnsupportedResource,
                "unhandled resource kind during binding",
            };
        }
    }

    for (size_t i = 0; i < m_built_graph->passes.size(); ++i) {
        m_built_graph->applyBarrierSlot(i, frame_ctx);

        BuiltFrameGraph::BuiltPass const& built_pass = m_built_graph->passes[i];
        FullPassOptions& fpo = m_passes[static_cast<size_t>(built_pass.id)];
        
        if (!fpo.callback) {
            throw FrameGraphError{ FrameGraphError::FramePassCallbackNotBound };    
        }

        if (built_pass.needs_render_scope) {
            RenderScope scope = frame_ctx.acquireRenderScope(
                built_pass.attachments,
                provider,
                built_pass.viewport,
                built_pass.scissor
            );
            fpo.callback(FramePassContext{cmd});
        } else {
            fpo.callback(FramePassContext{cmd});
        }

        if (!fpo.persistent) {
            fpo.callback = nullptr;
        }
    }

    m_built_graph->applyBarrierSlot(m_built_graph->barriers.size() - 1, frame_ctx);
}

} // namespace core::vk
