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
    bool operator==(TerrainLodChunkSource const&) const = default;
};

struct TerrainLodAuthoritySource final {
    shared::ChunkCoordinate coordinate;
    shared::TerrainIdentity identity;
    uint64_t revision = 0U;
    uint64_t content_hash = 0U;
    bool operator==(TerrainLodAuthoritySource const&) const = default;
};

struct TerrainLodColumnInput final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    std::vector<TerrainLodChunkInput> chunks;
    std::vector<TerrainLodChunkSource> sources;
    std::vector<shared::AuthoritativeTerrainColumn> authority_profiles;
    std::vector<TerrainLodAuthoritySource> authority_sources;
    std::array<std::array<uint16_t, 16>, 4> edge_heights{};
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
};

[[nodiscard]]
std::optional<TerrainLodColumnInput> captureTerrainLodColumn(shared::SparseWorld const& world, uint32_t x, uint32_t y);
[[nodiscard]]
bool terrainLodColumnSourcesCurrent(TerrainLodColumnInput const& column, shared::SparseWorld const& world);
[[nodiscard]]
bool terrainLodSourcesCurrent(TerrainLodScene const& scene, shared::SparseWorld const& world);
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
