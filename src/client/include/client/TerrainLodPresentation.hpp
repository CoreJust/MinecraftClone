#pragma once

#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace shared {
struct TerrainLodArtifact;
} // namespace shared

namespace client {

class TerrainLodDrawIndex;
struct TerrainLodRiverConstraints;
struct TerrainLodNormalContext;

// Visual exact-column centers use the existing full-detail disk. Canonical
// authority and the neighbor capture halo retain their separate coverage.
[[nodiscard]] constexpr uint32_t terrainLodVisualDistanceSquared(
    uint32_t const x, uint32_t const y, uint32_t const center_x, uint32_t const center_y) noexcept
{
    uint32_t const dx = x > center_x ? x - center_x : center_x - x;
    uint32_t const dy = y > center_y ? y - center_y : center_y - y;
    uint32_t const wx = std::min(dx, 4096U - dx);
    uint32_t const wy = std::min(dy, 4096U - dy);
    return wx * wx + wy * wy;
}

[[nodiscard]] constexpr bool terrainLodVisualColumn(
    uint32_t const x, uint32_t const y, uint32_t const center_x, uint32_t const center_y) noexcept
{
    return x < 4096U && y < 4096U && center_x < 4096U && center_y < 4096U
        && terrainLodVisualDistanceSquared(x, y, center_x, center_y)
            <= shared::FULL_DETAIL_RADIUS_CHUNKS * shared::FULL_DETAIL_RADIUS_CHUNKS;
}

[[nodiscard]] constexpr uint32_t terrainLodVisualColumnCount() noexcept
{
    uint32_t count = 0U;
    int32_t const radius = static_cast<int32_t>(shared::FULL_DETAIL_RADIUS_CHUNKS);
    for (int32_t y = -radius; y <= radius; ++y) {
        for (int32_t x = -radius; x <= radius; ++x) {
            count += x * x + y * y <= radius * radius ? 1U : 0U;
        }
    }
    return count;
}

struct TerrainLodDrawGroup final {
    StoneIndirectDraws::Range range;
    // Includes worker-prepared shared-vertex normals. Neighbor geometry changes
    // can replace this immutable payload even when the group's bounds stay equal.
    std::shared_ptr<std::vector<StoneFaceInstance> const> face_storage;
    uint32_t face_storage_first_instance = 0U;
    uint32_t origin_x = 0U;
    uint32_t origin_y = 0U;
    uint32_t extent = 0U;
    int32_t minimum_z = 0;
    int32_t maximum_z = 1;
};

struct TerrainLodChunkInput final {
    shared::Chunk chunk;
    shared::ChunkNeighborFaces neighbors;
};

struct TerrainLodChunkSource final {
    shared::ChunkCoordinate coordinate;
    shared::ChunkContentIdentity identity;
    uint64_t edit_revision = 0U;
    bool operator==(TerrainLodChunkSource const&) const = default;
};

using TerrainLodAuthoritySource = shared::AuthoritativeTileVersion;

struct TerrainLodEditSource final {
    shared::ChunkCoordinate coordinate;
    uint64_t revision = 0U;
    bool operator==(TerrainLodEditSource const&) const = default;
};

struct TerrainLodColumnInput final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    std::vector<TerrainLodChunkInput> chunks;
    std::vector<TerrainLodEditSource> edit_sources;
    std::vector<TerrainLodChunkSource> sources;
    std::vector<shared::AuthoritativeTerrainColumn> authority_profiles;
    std::vector<TerrainLodAuthoritySource> authority_sources;
    std::array<std::array<uint16_t, 16>, 4> edge_heights{};
    std::array<std::array<shared::Block, 16>, 4> edge_materials{};
};

struct TerrainLodColumnCoordinate final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    bool operator==(TerrainLodColumnCoordinate const&) const = default;
};

class TerrainLodCapturedColumn;

// One batch binds captured columns to an immutable authoritative terrain identity.
class TerrainLodAuthorityBatch final {
public:
    void reset() noexcept { m_identity_record.reset(); }
    [[nodiscard]] bool hasIdentity() const noexcept { return static_cast<bool>(m_identity_record); }
    [[nodiscard]] bool matches(shared::TerrainIdentity const& identity) const
    {
        return m_identity_record && *m_identity_record == identity;
    }

private:
    friend std::shared_ptr<TerrainLodCapturedColumn const> captureTerrainLodColumnForBatch(
        shared::SparseWorld const&,
        uint32_t,
        uint32_t,
        TerrainLodAuthorityBatch&
    );
    friend std::vector<TerrainLodColumnCoordinate> staleCapturedTerrainLodColumns(
        std::span<std::shared_ptr<TerrainLodCapturedColumn const> const>,
        shared::SparseWorld const&,
        TerrainLodAuthorityBatch const&
    );

    std::shared_ptr<shared::TerrainIdentity const> m_identity_record;
};

class TerrainLodCapturedColumn final {
public:
    class CaptureToken final {
    public:
        CaptureToken(CaptureToken const&) = default;

    private:
        CaptureToken() = default;
        friend std::shared_ptr<TerrainLodCapturedColumn const> captureTerrainLodColumnForBatch(
            shared::SparseWorld const&,
            uint32_t,
            uint32_t,
            TerrainLodAuthorityBatch&
        );
    };

    TerrainLodCapturedColumn(
        CaptureToken,
        TerrainLodColumnInput&& input,
        std::shared_ptr<shared::TerrainIdentity const> authority_identity_record
    );

    [[nodiscard]] TerrainLodColumnInput const& input() const noexcept { return m_input; }

private:
    friend std::vector<TerrainLodColumnCoordinate> staleCapturedTerrainLodColumns(
        std::span<std::shared_ptr<TerrainLodCapturedColumn const> const>,
        shared::SparseWorld const&,
        TerrainLodAuthorityBatch const&
    );

    TerrainLodColumnInput m_input;
    std::shared_ptr<shared::TerrainIdentity const> m_authority_identity_record;
};

struct TerrainLodActualColumn final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    std::vector<shared::ChunkMesh> meshes;
    std::vector<StoneFaceInstance> faces;
    std::array<std::array<uint16_t, 16>, 4> edge_heights{};
    std::array<std::array<shared::Block, 16>, 4> edge_materials{};
};

// Counts only newly generated geometry in this build. Reused immutable groups
// are not counted again.
struct TerrainLodRiverGeometryAudit final {
    double maximum_xy_quantization_error_m = 0.0;
    uint64_t original_top_triangle_count = 0U;
    uint64_t generated_top_triangle_count = 0U;
    uint64_t nearest_snap_top_triangle_reversal_count = 0U;
    uint64_t role_aware_top_triangle_reversal_count = 0U;
    uint64_t radial_steiner_lattice_adjustment_count = 0U;
    uint64_t inspected_fan_count = 0U;
    uint64_t diagnostic_failed_fan_count = 0U;
    uint64_t steiner_node_reassignment_count = 0U;
    uint64_t unconstrained_edge_flip_count = 0U;
    uint64_t constrained_cavity_retriangulation_count = 0U;
    uint64_t cavity_retriangulated_triangle_count = 0U;
    uint64_t cavity_output_triangle_count = 0U;
    uint64_t duplicate_renderer_node_quotient_count = 0U;
    int64_t top_face_count_change = 0;
    uint64_t snapped_top_triangle_reversal_count = 0U;
    uint64_t snapped_top_triangle_collapse_count = 0U;
    uint64_t emitted_bridge_triangle_count = 0U;
    uint64_t regional_export_build_count = 0U;
    uint64_t regional_export_reuse_count = 0U;
};

struct TerrainLodLocalLayout;
struct TerrainLodLocalUnit;

struct TerrainLodSelectionAudit final {
    size_t reused_l0_root_count = 0U;
    size_t rebuilt_l0_root_count = 0U;
};

struct TerrainLodScene final {
    // Worker-only incidence memberships; weak payload bindings retain no old scene or geometry.
    std::shared_ptr<TerrainLodNormalContext const> normal_context;
    shared::TerrainIdentity identity;
    uint64_t session_join_epoch = 0U;
    uint64_t coverage_epoch = 0U;
    uint32_t center_x = 0U;
    uint32_t center_y = 0U;
    // Exact presentation may follow the player while coarse reuse retains its center.
    std::optional<TerrainLodColumnCoordinate> visual_center;
    std::shared_ptr<generation::GlobalAtlas const> shading_atlas;
    std::shared_ptr<shared::TerrainLodArtifact const> root_artifact;
    std::shared_ptr<TerrainLodRiverConstraints const> river_constraints;
    TerrainLodRiverGeometryAudit river_geometry_audit;
    uint32_t root_extent = 256U;
    uint32_t block_sea_level = 0U;
    size_t logical_face_count = 0U;
    std::vector<TerrainLodDrawGroup> groups;
    std::vector<TerrainLodRegion> leaves;
    uint32_t actual_column_count = 0U;
    std::vector<TerrainLodChunkSource> chunk_sources;
    // Only fallback edit dependencies; authority-backed freshness uses tile versions.
    std::vector<TerrainLodEditSource> edit_sources;
    std::vector<TerrainLodAuthoritySource> authority_sources;
    std::shared_ptr<TerrainLodDrawIndex const> draw_index;
    // Immutable field/layout values only; never retains a parent scene or draw index.
    std::shared_ptr<TerrainLodLocalLayout const> local_layout;
    TerrainLodSelectionAudit selection_audit;
};

[[nodiscard]] std::span<StoneFaceInstance const> terrainLodGroupFaces(TerrainLodDrawGroup const& group);
[[nodiscard]] size_t terrainLodSceneFaceCount(TerrainLodScene const& scene) noexcept;

[[nodiscard]]
std::optional<TerrainLodColumnInput> captureTerrainLodColumn(shared::SparseWorld const& world, uint32_t x, uint32_t y);
[[nodiscard]]
std::shared_ptr<TerrainLodCapturedColumn const> captureTerrainLodColumnForBatch(
    shared::SparseWorld const& world,
    uint32_t x,
    uint32_t y,
    TerrainLodAuthorityBatch& batch);
[[nodiscard]]
bool terrainLodColumnSourcesCurrent(TerrainLodColumnInput const& column, shared::SparseWorld const& world);
[[nodiscard]]
// The runtime supplies the one identity captured for this homogeneous batch.
std::vector<TerrainLodColumnCoordinate> staleTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    shared::SparseWorld const& world,
    shared::TerrainIdentity const& captured_batch_identity
);
[[nodiscard]]
std::vector<TerrainLodColumnCoordinate> staleCapturedTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> columns,
    shared::SparseWorld const& world,
    TerrainLodAuthorityBatch const& batch);
[[nodiscard]]
bool terrainLodSourcesCurrent(TerrainLodScene const& scene, shared::SparseWorld const& world);
struct TerrainLodLocalPreparation final {
    std::shared_ptr<TerrainLodScene const> scene;
    std::shared_ptr<TerrainLodLocalUnit const> unit;
};

// Rebuilds exact footprints and their closed transition unit, retaining other immutable groups.
// Unsupported field/layout/mask contexts return an empty result for the normal scene fallback.
// Inputs are immutable captures; workers never inspect SparseWorld.
[[nodiscard]] TerrainLodLocalPreparation prepareTerrainLodLocalScene(
    shared::TerrainWorld const& world,
    TerrainLodScene const& base,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> base_columns,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::span<TerrainLodColumnCoordinate const> targets,
    std::span<std::shared_ptr<TerrainLodLocalUnit const> const> constraints = {},
    std::stop_token const& cancellation = {},
    bool transient_recenter = false);
[[nodiscard]] bool terrainLodLocalSourcesCurrent(TerrainLodLocalUnit const& unit, shared::SparseWorld const& world);
[[nodiscard]] std::span<TerrainLodColumnCoordinate const> terrainLodLocalTargets(TerrainLodLocalUnit const& unit);

[[nodiscard]]
std::shared_ptr<TerrainLodScene const> buildTerrainLodColumnPatch(
    shared::TerrainIdentity identity,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::stop_token const& cancellation = {});
[[nodiscard]]
// With previous, unchanged must be captures already rendered by that scene.
// Their exact groups are reused; complete membership still supplies rim context.
// Promotion is explicit; previous may also supply geometry for a new center.
// Promotion refines new exact footprints and their 32-block transition band,
// including destinations outside the original center's coarse selection.
// The optional prepared root comes from the same source publication as world.
std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneFromColumns(
    shared::TerrainWorld const& world,
    uint32_t center_x,
    uint32_t center_y,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::stop_token const& cancellation = {},
    TerrainLodScene const* previous = nullptr,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> unchanged = {},
    bool promote_columns = false,
    std::optional<TerrainLodColumnCoordinate> visual_center = std::nullopt,
    std::shared_ptr<shared::TerrainLodArtifact const> const& prepared_root = {});
[[nodiscard]]
std::shared_ptr<TerrainLodScene const> buildTerrainLodScene(
    shared::TerrainWorld const& world,
    uint32_t center_x,
    uint32_t center_y,
    std::span<TerrainLodActualColumn const> actual_columns = {},
    std::stop_token const& cancellation = {},
    TerrainLodScene const* previous = nullptr,
    std::span<TerrainLodColumnCoordinate const> changed = {},
    bool promote_columns = false,
    std::optional<TerrainLodColumnCoordinate> visual_center = std::nullopt);

} // namespace client
