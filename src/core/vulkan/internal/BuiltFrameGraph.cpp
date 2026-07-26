#include <core/vulkan/internal/BuiltFrameGraph.hpp>

#include <core/algorithm/TopoSort.hpp>
#include <core/common/Assert.hpp>
#include <core/vulkan/enum/AccessFlag.hpp>
#include <core/vulkan/enum/ImageLayout.hpp>
#include <core/vulkan/enum/PipelineStage.hpp>

#include <algorithm>

namespace core::vk {

enum class ResourceUsage {
    Undefined,
    ColorAttachment,
    DepthStencilAttachment,
    InputAttachment,
    ColorResolve,
    DepthStencilResolve,
    Preserve,
};

namespace {

[[nodiscard]] constexpr bool isWriteUsage(ResourceUsage const usage) noexcept {
    return false
        || usage == ResourceUsage::ColorAttachment
        || usage == ResourceUsage::DepthStencilAttachment
        || usage == ResourceUsage::ColorResolve
        || usage == ResourceUsage::DepthStencilResolve;
}

} // namespace

struct PassAttachmentData {
    Attachments attachments;
    std::vector<ResourceUsage> usages;
};

struct FrameGraph::BuiltFrameGraph::FrameGraphBuilder final {
    std::span<FullPassOptions const> const passes;
    std::span<FrameResource const> const resources;
    std::span<AttachmentViewId const> const view_ids;
    BuiltFrameGraph& graph;

    std::vector<size_t> sorted_passes;
    std::vector<size_t> first_write;
    std::vector<PassAttachmentData> per_pass;

    FrameGraphBuilder(
        std::span<FullPassOptions const> const passes,
        std::span<FrameResource const> const resources,
        std::span<AttachmentViewId const> const view_ids,
        BuiltFrameGraph& graph
    ) noexcept
        : passes(passes)
        , resources(resources)
        , view_ids(view_ids)
        , graph(graph)
    { }

    void build() {
        sortPasses();
        computeFirstWrites();
        buildPassAttachmentData();
        buildBarriers();
        buildPasses();
    }

private:
    void sortPasses() {
        auto const maybe_sorted = core::topoSort(
            passes.size(),
            [this](size_t const node) -> auto const& { return passes[node].options.dependencies; }
        );
        if (!maybe_sorted.has_value()) {
            throw FrameGraphError{
                FrameGraphError::CyclicDependency,
                "cycle detected in frame graph",
            };
        }
        sorted_passes = std::move(*maybe_sorted);
    }

    void computeFirstWrites() {
        size_t const res_count = resources.size();
        first_write.assign(res_count, SIZE_MAX);
        for (size_t order = 0; order < sorted_passes.size(); ++order) {
            size_t const pass_idx = sorted_passes[order];
            for (FrameResourceId const res_id : passes[pass_idx].options.written_resources) {
                size_t const res_idx = static_cast<size_t>(res_id);
                if (first_write[res_idx] == SIZE_MAX) {
                    first_write[res_idx] = order;
                }
            }
        }
    }

    void buildPassAttachmentData() {
        size_t const res_count = resources.size();
        per_pass.reserve(sorted_passes.size());

        for (size_t order = 0; order < sorted_passes.size(); ++order) {
            size_t const pass_idx = sorted_passes[order];
            FramePassOptions const& pass_options = passes[pass_idx].options;

            // Detect resources that are both read and written in the same pass.
            for (FrameResourceId const res_id : pass_options.read_resources) {
                if (std::ranges::contains(pass_options.written_resources, res_id)) {
                    throw FrameGraphError{
                        FrameGraphError::UnsupportedResource,
                        "resource cannot be both read and written in the same pass",
                    };
                }
            }

            PassAttachmentData attachment_data{};
            attachment_data.usages.resize(res_count, ResourceUsage::Undefined);
            Attachments& attachments = attachment_data.attachments;

            for (FrameResourceId const res_id : pass_options.written_resources) {
                size_t const res_idx = static_cast<size_t>(res_id);
                bool const is_first = (first_write[res_idx] == order);
                bool const do_clear = is_first && resources[res_idx].clear_color.has_value();
                attachments.colors.emplace_back(
                    do_clear
                        ? ColorAttachment::clear(view_ids[res_idx], *resources[res_idx].clear_color)
                        : ColorAttachment::load(view_ids[res_idx])
                );
                attachment_data.usages[res_idx] = ResourceUsage::ColorAttachment;
            }

            for (FrameResourceId const res_id : pass_options.read_resources) {
                size_t const res_idx = static_cast<size_t>(res_id);
                if (attachment_data.usages[res_idx] != ResourceUsage::Undefined) {
                    continue;
                }
                attachments.inputs.emplace_back(InputAttachment{view_ids[res_idx]});
                attachment_data.usages[res_idx] = ResourceUsage::InputAttachment;
            }

            per_pass.emplace_back(std::move(attachment_data));
        }
    }

    void buildBarriers() {
        size_t const res_count = resources.size();
        std::vector<ResourceUsage> current(res_count, ResourceUsage::Undefined);
        graph.barriers.resize(sorted_passes.size() + 1);

        for (size_t i = 0; i < sorted_passes.size(); ++i) {
            auto& slot = graph.barriers[i];
            for (size_t res_idx = 0; res_idx < res_count; ++res_idx) {
                ResourceUsage const target = per_pass[i].usages[res_idx];
                if (target == ResourceUsage::Undefined) {
                    continue;
                }
                std::optional<ImageMemoryBarrier> barrier = makeTransitionBarrier(
                    resources[res_idx].kind,
                    current[res_idx],
                    target
                );
                if (barrier.has_value()) {
                    slot.barriers.push_back(*barrier);
                    slot.view_ids.push_back(view_ids[res_idx]);
                }
                current[res_idx] = target;
            }
        }

        auto& final_slot = graph.barriers.back();
        for (size_t res_idx = 0; res_idx < res_count; ++res_idx) {
            if (resources[res_idx].kind != FrameResource::Kind::Swapchain) {
                continue;
            }
            if (current[res_idx] == ResourceUsage::Undefined) {
                continue;
            }
            final_slot.barriers.push_back(makeSwapchainPresentBarrier(current[res_idx]));
            final_slot.view_ids.push_back(view_ids[res_idx]);
        }
    }

    void buildPasses() {
        graph.passes.reserve(sorted_passes.size());
        for (size_t order = 0; order < sorted_passes.size(); ++order) {
            size_t const pass_idx = sorted_passes[order];
            Attachments const& attachments = per_pass[order].attachments;

            graph.passes.push_back(BuiltPass{
                .id = FramePassId{static_cast<uint32_t>(pass_idx)},
                .attachments = attachments,
                .viewport = passes[pass_idx].options.viewport,
                .scissor = passes[pass_idx].options.scissor,
                .needs_render_scope = false
                    || !attachments.colors.empty()
                    || attachments.depth_stencil.has_value()
                    || !attachments.inputs.empty()
                    || !attachments.color_resolves.empty()
                    || attachments.depth_stencil_resolve.has_value(),
            });
        }
    }

    [[nodiscard]]
    static std::optional<ImageMemoryBarrier> makeTransitionBarrier(
        FrameResource::Kind const kind,
        ResourceUsage const from,
        ResourceUsage const to
    ) {
        if (from == to) {
            if (isWriteUsage(from)) {
                return ImageMemoryBarrier{
                    .src_stages = stageForUsage(kind, from),
                    .dst_stages = stageForUsage(kind, to),
                    .src_access = accessForUsage(kind, from),
                    .dst_access = accessForUsage(kind, to),
                    .old_layout = layoutForUsage(kind, from),
                    .new_layout = layoutForUsage(kind, to),
                };
            }
            return std::nullopt;
        }

        if (kind == FrameResource::Kind::Swapchain) {
            return ImageMemoryBarrier{
                .src_stages = stageForUsage(kind, from),
                .dst_stages = stageForUsage(kind, to),
                .src_access = accessForUsage(kind, from),
                .dst_access = accessForUsage(kind, to),
                .old_layout = layoutForUsage(kind, from),
                .new_layout = layoutForUsage(kind, to),
            };
        }

        throw FrameGraphError{
            FrameGraphError::UnsupportedResource,
            "unsupported resource kind in barrier generation",
        };
    }

    [[nodiscard]]
    static ImageMemoryBarrier makeSwapchainPresentBarrier(ResourceUsage const current_usage) {
        ImageMemoryBarrier barrier{
            .dst_stages = PipelineStage::BottomOfPipe,
            .new_layout = ImageLayout::PresentSrc,
        };
        switch (current_usage) {
            case ResourceUsage::ColorAttachment:
                barrier.src_stages = PipelineStage::ColorAttachmentOutput;
                barrier.src_access = AccessFlag::ColorAttachmentWrite;
                barrier.old_layout = ImageLayout::ColorAttachmentOptimal;
                break;
            case ResourceUsage::InputAttachment:
                barrier.src_stages = PipelineStage::FragmentShader;
                barrier.src_access = AccessFlag::ShaderRead;
                barrier.old_layout = ImageLayout::ShaderReadOnlyOptimal;
                break;
            default:
                throw FrameGraphError{
                    FrameGraphError::UnsupportedMemoryBarrierStagesValue,
                    "invalid final state for swapchain presentation",
                };
        }
        return barrier;
    }

    [[nodiscard]]
    static PipelineStages stageForUsage(FrameResource::Kind const kind, ResourceUsage const usage) {
        if (kind == FrameResource::Kind::Swapchain) {
            switch (usage) {
                case ResourceUsage::Undefined: return PipelineStage::TopOfPipe;
                case ResourceUsage::ColorAttachment: return PipelineStage::ColorAttachmentOutput;
                case ResourceUsage::InputAttachment: return PipelineStage::FragmentShader;
                default: throw FrameGraphError{FrameGraphErrorKind::UnsupportedMemoryBarrierStagesValue};
            }
        }
        throw FrameGraphError{FrameGraphErrorKind::UnsupportedResource};
    }

    [[nodiscard]]
    static AccessFlags accessForUsage(FrameResource::Kind const kind, ResourceUsage const usage) {
        if (kind == FrameResource::Kind::Swapchain) {
            switch (usage) {
                case ResourceUsage::Undefined: return AccessFlags::None;
                case ResourceUsage::ColorAttachment: return AccessFlag::ColorAttachmentWrite;
                case ResourceUsage::InputAttachment: return AccessFlag::ShaderRead;
            default: throw FrameGraphError{FrameGraphErrorKind::UnsupportedMemoryBarrierAccessValue};
            }
        }
        throw FrameGraphError{FrameGraphErrorKind::UnsupportedResource};
    }

    [[nodiscard]]
    static ImageLayout layoutForUsage(FrameResource::Kind const kind, ResourceUsage const usage) {
        if (kind == FrameResource::Kind::Swapchain) {
            switch (usage) {
                case ResourceUsage::Undefined: return ImageLayout::Undefined;
                case ResourceUsage::ColorAttachment: return ImageLayout::ColorAttachmentOptimal;
                case ResourceUsage::InputAttachment: return ImageLayout::ShaderReadOnlyOptimal;
            default: throw FrameGraphError{FrameGraphErrorKind::UnsupportedMemoryBarrierStagesValue};
            }
        }
        throw FrameGraphError{FrameGraphErrorKind::UnsupportedResource};
    }
};

void FrameGraph::BuiltFrameGraph::BarrierSlot::apply(
    FrameContext& frame_ctx,
    AttachmentViewProvider const& provider,
    std::vector<RawImage>& scratch_images
) {
    ASSERT(barriers.size() == view_ids.size());
    scratch_images.clear();
    scratch_images.reserve(barriers.size());
    for (size_t i = 0; i < barriers.size(); ++i) {
        scratch_images.push_back(provider[view_ids[i]].image);
    }
    frame_ctx.setImageBarriers(barriers, scratch_images);
}

std::unique_ptr<FrameGraph::BuiltFrameGraph> FrameGraph::BuiltFrameGraph::create(
    std::span<FullPassOptions const> const passes,
    std::span<FrameResource const> const resources,
    std::span<AttachmentViewId const> const resources_view_ids,
    AttachmentViewProvider&& provider
) {
    auto graph = std::make_unique<BuiltFrameGraph>();
    graph->provider = std::move(provider);
    FrameGraphBuilder builder{ passes, resources, resources_view_ids, *graph };
    builder.build();
    return graph;
}

} // namespace core::vk
