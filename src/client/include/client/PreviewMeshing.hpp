#pragma once

#include <client/PreviewResidency.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace client {

inline constexpr size_t MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT = 4U;

enum class PreviewMeshStage : uint8_t {
    Coarse,
    Final,
};

[[nodiscard]] inline shared::HeightTileSurfaceDetail previewMeshCoarseDetail(
    shared::HeightTileSurfaceDetail const detail
) noexcept
{
    return detail;
}

[[nodiscard]] inline PreviewMeshStage previewMeshStageForTile(
    HeightTileHandle const& visible_tile,
    HeightTileHandle const& current_tile,
    shared::HeightTileSurfaceDetail const target_detail
) noexcept
{
    if (visible_tile == current_tile || previewMeshCoarseDetail(target_detail) == target_detail) {
        return PreviewMeshStage::Final;
    }
    return PreviewMeshStage::Coarse;
}

[[nodiscard]] inline bool previewMeshCanPublish(
    HeightTileHandle const& visible_tile,
    HeightTileHandle const& candidate_tile,
    PreviewMeshStage const candidate_stage,
    HeightTileHandle const& current_tile
) noexcept
{
    if (!candidate_tile || candidate_tile != current_tile) {
        return false;
    }
    return visible_tile != candidate_tile || candidate_stage == PreviewMeshStage::Final;
}

struct PreviewMeshSeamBridgeSet final {
    static constexpr std::array<size_t, 4> OPPOSITE_EDGES{ 1U, 0U, 3U, 2U };
    static constexpr std::array<shared::HeightTileSurfaceDirection, 4> EDGES{
        shared::HeightTileSurfaceDirection::NegativeX,
        shared::HeightTileSurfaceDirection::PositiveX,
        shared::HeightTileSurfaceDirection::NegativeY,
        shared::HeightTileSurfaceDirection::PositiveY,
    };

    [[nodiscard]] bool replace(size_t const edge, std::vector<shared::HeightTileSurfaceQuad> bridges)
    {
        if (m_edges.at(edge) == bridges) {
            return false;
        }
        m_edges[edge] = std::move(bridges);
        return true;
    }

    [[nodiscard]] std::array<std::vector<shared::HeightTileSurfaceQuad>, 4> const& edges() const noexcept
    {
        return m_edges;
    }

    [[nodiscard]] shared::HeightTileSurfaceMesh compose(
        shared::HeightTileSurfaceMesh const& base_mesh
    ) const
    {
        shared::HeightTileSurfaceMesh mesh = base_mesh;
        for (std::vector<shared::HeightTileSurfaceQuad> const& bridges : m_edges) {
            mesh.quads.insert(mesh.quads.end(), bridges.begin(), bridges.end());
        }
        return mesh;
    }

private:
    std::array<std::vector<shared::HeightTileSurfaceQuad>, 4> m_edges;
};

struct PreviewMeshSeamUpdate final {
    bool first_changed = false;
    bool second_changed = false;
};

struct PreviewMeshSeamNeighbor final {
    shared::HeightTileSurfaceMesh const* mesh = nullptr;
    PreviewMeshSeamBridgeSet* bridges = nullptr;
};

struct PreviewMeshSeamNeighborUpdates final {
    std::array<bool, 4> changed{};
    size_t count = 0U;
};

struct PreviewMeshSeamNeighborSnapshot final {
    shared::HeightTileSurfaceMesh const* mesh = nullptr;
    PreviewMeshSeamBridgeSet const* bridges = nullptr;
};

struct PreviewMeshSeamPublicationPlan final {
    shared::HeightTileSurfaceMesh center_mesh;
    PreviewMeshSeamBridgeSet center_bridges;
    std::array<PreviewMeshSeamBridgeSet, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> neighbor_bridges;
    std::array<bool, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> changed_neighbors{};
};

struct PreviewMeshSeamPublicationBatch final {
    std::array<shared::HeightTileSurfaceMesh, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT + 1U> meshes;
    uint32_t count = 0U;
};

[[nodiscard]] inline PreviewMeshSeamUpdate updatePreviewMeshSeamBridges(
    shared::HeightTileSurfaceMesh const& first,
    size_t const first_edge,
    PreviewMeshSeamBridgeSet& first_bridges,
    shared::HeightTileSurfaceMesh const& second,
    PreviewMeshSeamBridgeSet& second_bridges
)
{
    size_t const second_edge = PreviewMeshSeamBridgeSet::OPPOSITE_EDGES.at(first_edge);
    shared::HeightTileSurfaceSeamBridge const bridge = shared::heightTileSurfaceSeamBridge(
        first,
        second,
        PreviewMeshSeamBridgeSet::EDGES.at(first_edge)
    );
    return {
        .first_changed = first_bridges.replace(first_edge, bridge.first),
        .second_changed = second_bridges.replace(second_edge, bridge.second),
    };
}

[[nodiscard]] inline PreviewMeshSeamNeighborUpdates updatePreviewMeshSeamBridgesForVisibleNeighbors(
    shared::HeightTileSurfaceMesh const& base_mesh,
    PreviewMeshSeamBridgeSet& base_bridges,
    std::array<PreviewMeshSeamNeighbor, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> const& neighbors
)
{
    PreviewMeshSeamNeighborUpdates updates;
    for (size_t edge = 0U; edge < neighbors.size(); ++edge) {
        PreviewMeshSeamNeighbor const& neighbor = neighbors[edge];
        if (neighbor.mesh == nullptr || neighbor.bridges == nullptr) {
            static_cast<void>(base_bridges.replace(edge, {}));
            continue;
        }
        PreviewMeshSeamUpdate const update = updatePreviewMeshSeamBridges(
            base_mesh,
            edge,
            base_bridges,
            *neighbor.mesh,
            *neighbor.bridges
        );
        updates.changed[edge] = update.second_changed;
        updates.count += update.second_changed ? 1U : 0U;
    }
    return updates;
}

[[nodiscard]]
inline PreviewMeshSeamPublicationPlan planPreviewMeshSeamPublication(
    shared::HeightTileSurfaceMesh const& center_mesh,
    PreviewMeshSeamBridgeSet const& center_bridges,
    std::array<PreviewMeshSeamNeighborSnapshot, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> const& neighbors
)
{
    PreviewMeshSeamPublicationPlan plan{
        .center_bridges = center_bridges,
    };
    for (uint32_t edge = 0U; edge < neighbors.size(); ++edge) {
        PreviewMeshSeamNeighborSnapshot const& neighbor = neighbors[edge];
        if (neighbor.mesh == nullptr) {
            static_cast<void>(plan.center_bridges.replace(edge, {}));
            continue;
        }
        if (neighbor.bridges != nullptr) {
            plan.neighbor_bridges[edge] = *neighbor.bridges;
        }
        PreviewMeshSeamUpdate const update = updatePreviewMeshSeamBridges(
            center_mesh,
            edge,
            plan.center_bridges,
            *neighbor.mesh,
            plan.neighbor_bridges[edge]
        );
        plan.changed_neighbors[edge] = update.second_changed;
    }
    plan.center_mesh = plan.center_bridges.compose(center_mesh);
    return plan;
}

[[nodiscard]]
inline PreviewMeshSeamPublicationBatch buildPreviewMeshSeamPublicationBatch(
    PreviewMeshSeamPublicationPlan const& plan,
    std::array<PreviewMeshSeamNeighborSnapshot, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> const& neighbors
)
{
    PreviewMeshSeamPublicationBatch batch;
    batch.meshes[batch.count++] = plan.center_mesh;
    for (uint32_t edge = 0U; edge < neighbors.size(); ++edge) {
        if (!plan.changed_neighbors[edge] || neighbors[edge].mesh == nullptr) {
            continue;
        }
        batch.meshes[batch.count++] = plan.neighbor_bridges[edge].compose(*neighbors[edge].mesh);
    }
    return batch;
}

template <typename Upload, typename Commit>
requires requires(
    Upload&& upload,
    Commit&& commit,
    std::span<shared::HeightTileSurfaceMesh const> meshes,
    PreviewMeshSeamPublicationPlan& plan
) {
    { std::forward<Upload>(upload)(meshes) } -> std::same_as<bool>;
    std::forward<Commit>(commit)(plan);
}
[[nodiscard]]
inline bool publishPreviewMeshSeamPlan(
    PreviewMeshSeamPublicationPlan& plan,
    PreviewMeshSeamPublicationBatch const& batch,
    Upload&& upload,
    Commit&& commit
)
{
    std::span<shared::HeightTileSurfaceMesh const> const meshes{ batch.meshes.data(), batch.count };
    if (!std::forward<Upload>(upload)(meshes)) {
        return false;
    }
    std::forward<Commit>(commit)(plan);
    return true;
}

struct PreviewMeshSource final {
    HeightTileHandle tile;
    std::array<HeightTileHandle, 4> neighbors;
    shared::HeightTileSurfaceDetail detail = shared::HeightTileSurfaceDetail::Fine;
    std::array<shared::HeightTileSurfaceDetail, 4> neighbor_details{
        shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine,
    };
};

[[nodiscard]] inline shared::HeightTileSurfaceMesh buildPreviewMesh(PreviewMeshSource const& source)
{
    auto const heights = [](HeightTileHandle const& tile) -> std::optional<shared::HeightTile::Heights> {
        return tile ? std::optional<shared::HeightTile::Heights>{tile->heights()} : std::nullopt;
    };
    shared::HeightTileSurfaceNeighbors const neighbors{
        .negative_x = heights(source.neighbors[0]),
        .positive_x = heights(source.neighbors[1]),
        .negative_y = heights(source.neighbors[2]),
        .positive_y = heights(source.neighbors[3]),
        .negative_x_detail = source.neighbor_details[0],
        .positive_x_detail = source.neighbor_details[1],
        .negative_y_detail = source.neighbor_details[2],
        .positive_y_detail = source.neighbor_details[3],
    };
    shared::HeightTileSurfaceMesher mesher;
    return mesher.build({
        .coordinate = {.x = source.tile->key().x, .y = source.tile->key().y},
        .heights = source.tile->heights(),
    }, neighbors, source.detail);
}

} // namespace client
