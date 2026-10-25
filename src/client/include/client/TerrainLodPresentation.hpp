#pragma once

#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace client {

class TerrainLodDrawIndex;

struct TerrainLodDrawGroup final {
    StoneIndirectDraws::Range range;
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
};

struct TerrainLodScene final {
    shared::TerrainIdentity identity;
    uint64_t session_join_epoch = 0U;
    uint64_t coverage_epoch = 0U;
    uint32_t center_x = 0U;
    uint32_t center_y = 0U;
    std::shared_ptr<generation::GlobalAtlas const> shading_atlas;
    uint32_t root_extent = 256U;
    uint32_t block_sea_level = 0U;
    std::vector<StoneFaceInstance> faces;
    std::vector<TerrainLodDrawGroup> groups;
    std::vector<TerrainLodRegion> leaves;
    uint32_t actual_column_count = 0U;
    std::vector<TerrainLodChunkSource> chunk_sources;
    std::vector<TerrainLodAuthoritySource> authority_sources;
    std::shared_ptr<TerrainLodDrawIndex const> draw_index;
};

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
[[nodiscard]]
std::shared_ptr<TerrainLodScene const> buildTerrainLodColumnPatch(
    shared::TerrainIdentity identity,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::stop_token const& cancellation = {});
[[nodiscard]]
std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneFromColumns(
    shared::TerrainWorld const& world,
    uint32_t center_x,
    uint32_t center_y,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::stop_token const& cancellation = {});
[[nodiscard]]
std::shared_ptr<TerrainLodScene const> buildTerrainLodScene(
    shared::TerrainWorld const& world,
    uint32_t center_x,
    uint32_t center_y,
    std::span<TerrainLodActualColumn const> actual_columns = {},
    std::stop_token const& cancellation = {});

} // namespace client
