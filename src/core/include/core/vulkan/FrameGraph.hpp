#pragma once

#include <core/common/Color.hpp>
#include <core/meta/TaggedBool.hpp>
#include <core/vulkan/Context.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/FramePass.hpp>

#include <memory>

CORE_VK_ERROR_WITH_KINDS(FrameGraphError, VulkanRuntimeError,
    InvalidPassId,
    InvalidResourceId,
    CyclicDependency,
    UnsupportedResource,
    UnsupportedMemoryBarrierStagesValue,
    UnsupportedMemoryBarrierAccessValue,
    FailedToCreateRenderPass,
    FailedToCreateFramebuffer,
    FramePassCallbackNotBound);

namespace core::vk {

struct FramePassContext final {
    RawCommandBuffer cmd;
};

using FramePassBindCallback = std::function<void(FramePassContext)>;
using PersistentFramePassBindCallback = TaggedBool<struct PersistentFramePassBindCallbackTag>;

class FrameGraph final : NonCopyable {
public:
    explicit FrameGraph(VulkanContext&& ctx);
    FrameGraph(FrameGraph&&) noexcept;
    ~FrameGraph();

    [[nodiscard]]
    FrameResourceId importSwapchain(std::optional<Color4> const clear_color = std::nullopt);

    FramePassId add(FramePassOptions&& options);

    // Callback must not be null
    void bind(
        FramePassId const id,
        FramePassBindCallback&& cb,
        PersistentFramePassBindCallback const persistent = PersistentFramePassBindCallback::No
    );

    // Releases Vulkan resources, will cause a rebuild next time.
    void discard();

    // Renders, builds the graph if necessary.
    void render();
    
    void onReload(std::function<void(ReloadType const, ReloadSource const, ReloadAction const)>&& callback) {
        m_ctx.onReload(std::move(callback));
    }
    void reload(ReloadType const type) { m_ctx.reload(type); }
    void rebuild(auto&& fn, ReloadType const type = ReloadType::Instance) {
        m_ctx.rebuild(std::move(fn), type);
    }

    [[nodiscard]]
    VulkanContext& ctx() noexcept { return m_ctx; }
private:
    void build();
private:
    class BuiltFrameGraph;

    struct FullPassOptions final {
        FramePassOptions options;
        FramePassBindCallback callback;
        PersistentFramePassBindCallback persistent;
    };

    struct FrameResource final {
        enum class Kind {
            Swapchain,
        };

        std::optional<Color4> clear_color{ };
        Kind kind = Kind::Swapchain;
    };
private:
    VulkanContext m_ctx;
    std::vector<FullPassOptions> m_passes;
    std::vector<FrameResource> m_resources;
    std::vector<AttachmentViewId> m_resources_view_ids;
    std::unique_ptr<BuiltFrameGraph> m_built_graph;
};

} // namespace core::vk
