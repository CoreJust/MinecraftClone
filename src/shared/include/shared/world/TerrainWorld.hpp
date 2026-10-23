#pragma once

#include <shared/world/Chunk.hpp>

#include <generation/Generation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace shared {

inline constexpr std::array<uint32_t, 5> TERRAIN_REGION_EXTENTS{
    16'384U,
    4'096U,
    1'024U,
    256U,
    64U,
};

inline constexpr uint32_t TERRAIN_REGION_NODE_SIDE_LENGTH = 65U;
inline constexpr uint32_t TERRAIN_REGION_NODE_COUNT =
    TERRAIN_REGION_NODE_SIDE_LENGTH * TERRAIN_REGION_NODE_SIDE_LENGTH;
inline constexpr uint32_t TERRAIN_CONFIGURATION_IDENTITY_MAX_BYTES = 320U;

struct TerrainIdentity final {
    uint32_t terrain_version = generation::TERRAIN_VERSION;
    uint64_t revision = 1U;
    uint64_t seed = 0U;
    std::string configuration_identity;

    bool operator==(TerrainIdentity const&) const = default;
};

struct TerrainRegionKey final {
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t extent;

    constexpr bool operator==(TerrainRegionKey const&) const noexcept = default;
};

struct TerrainClimateSample final {
    double temperature_celsius = 0.0;
    double precipitation = 0.0;
};

struct TerrainColumn {
    uint16_t highest_solid_z = 0U;
    uint16_t water_height_z = 0U;
    bool water_present = false;
    bool submerged = false;
    double geological_elevation_km = 0.0;
    double temperature_celsius = 0.0;
    double precipitation = 0.0;
    Block top_material = Block::Dirt;
    Block ground_material = Block::Dirt;
    uint16_t ground_first_z = 0U;
    uint16_t ground_end_z = 0U;

    [[nodiscard]]
    Block materialAt(uint16_t z, uint32_t block_sea_level) const noexcept;
};

struct RegionalSample final : TerrainColumn {
    double world_x = 0.0;
    double world_y = 0.0;
};

class RegionalArtifact final {
public:
    [[nodiscard]]
    TerrainRegionKey const& key() const noexcept { return m_key; }

    [[nodiscard]]
    std::shared_ptr<RegionalArtifact const> parent() const noexcept { return m_parent; }

    [[nodiscard]]
    TerrainIdentity const& identity() const noexcept { return m_identity; }

    [[nodiscard]]
    static constexpr uint32_t sampleCount() noexcept { return TERRAIN_REGION_NODE_COUNT; }

private:
    struct Node final {
        RegionalSample sample;
        double base_elevation = 0.0;
        double accumulated_detail = 0.0;
        double last_contribution = 0.0;
    };

    [[nodiscard]]
    Node evaluate(double world_x, double world_y) const;

    [[nodiscard]]
    bool sampleIndex(double world_x, double world_y, uint32_t& index) const noexcept;

    friend class TerrainWorld;

private:
    TerrainRegionKey m_key{};
    uint32_t m_level_index = 0U;
    TerrainIdentity m_identity;
    std::shared_ptr<generation::GlobalAtlas const> m_atlas;
    std::shared_ptr<RegionalArtifact const> m_parent;
    std::array<Node, TERRAIN_REGION_NODE_COUNT> m_nodes{};
    std::array<bool, TERRAIN_REGION_NODE_COUNT> m_node_ready{};
};

struct SurfaceTile final {
    ChunkCoordinate coordinate{};
    uint32_t block_sea_level = generation::BLOCK_SEA_LEVEL;
    uint32_t max_height = generation::MAX_HEIGHT;
    TerrainIdentity identity;
    std::array<TerrainColumn, Chunk::FACE_BLOCK_COUNT> columns{};

    [[nodiscard]]
    TerrainColumn const* columnAt(BlockCoordinate coordinate) const noexcept;
};

[[nodiscard]]
bool isValidTerrainIdentity(TerrainIdentity const& identity) noexcept;

[[nodiscard]]
bool isValidTerrainColumn(
    TerrainColumn const& column,
    uint32_t block_sea_level,
    uint32_t max_height = generation::MAX_HEIGHT
) noexcept;

[[nodiscard]]
bool isValidSurfaceTile(SurfaceTile const& tile) noexcept;

class TerrainWorld final {
public:
    explicit TerrainWorld(
        std::shared_ptr<generation::GlobalAtlas const> atlas,
        uint64_t revision = 1U
    );

    [[nodiscard]]
    TerrainIdentity const& identity() const noexcept { return m_identity; }

    [[nodiscard]]
    std::shared_ptr<generation::GlobalAtlas const> atlas() const noexcept { return m_atlas; }

    // Matches actual-column climate at finite periodic world XY, without local altitude adjustment.
    [[nodiscard]]
    TerrainClimateSample sampleClimate(double world_x, double world_y) const;

    // Samples one canonical R6 solid-column height at periodic block XY.
    [[nodiscard]]
    uint16_t sampleSurfaceHeight(uint32_t world_x, uint32_t world_y) const;

    [[nodiscard]]
    std::shared_ptr<RegionalArtifact const> prepareRegion(
        TerrainRegionKey key,
        std::stop_token const& cancellation = {}
    ) const;

    // The coarsest extent takes no parent; each child requires the matching preceding extent.
    [[nodiscard]]
    std::shared_ptr<RegionalArtifact const> refineRegion(
        TerrainRegionKey key,
        std::shared_ptr<RegionalArtifact const> exact_parent,
        std::stop_token const& cancellation = {}
    ) const;

    [[nodiscard]]
    RegionalSample sampleRegion(
        RegionalArtifact const& region,
        double world_x,
        double world_y,
        std::stop_token const& cancellation = {}
    ) const;

    [[nodiscard]]
    SurfaceTile sampleSurface(
        RegionalArtifact const& leaf,
        ChunkCoordinate coordinate,
        std::stop_token const& cancellation = {}
    ) const;

    [[nodiscard]]
    SurfaceTile sampleSurface(
        ChunkCoordinate coordinate,
        std::stop_token const& cancellation = {}
    ) const;

    [[nodiscard]]
    Chunk materializeChunk(
        SurfaceTile const& tile,
        ChunkCoordinate coordinate,
        std::stop_token const& cancellation = {}
    ) const;

private:
    [[nodiscard]]
    std::shared_ptr<RegionalArtifact const> buildRegion(
        TerrainRegionKey key,
        uint32_t level_index,
        std::shared_ptr<RegionalArtifact const> parent,
        std::stop_token const& cancellation
    ) const;

    [[nodiscard]]
    std::shared_ptr<RegionalArtifact const> cachedRegion(TerrainRegionKey key) const;

    [[nodiscard]]
    std::shared_ptr<RegionalArtifact const> cacheRegion(
        std::shared_ptr<RegionalArtifact const> region
    ) const;

    struct RegionKeyHash final {
        [[nodiscard]]
        size_t operator()(TerrainRegionKey key) const noexcept;
    };

    struct RegionCacheEntry final {
        std::shared_ptr<RegionalArtifact const> region;
        std::list<TerrainRegionKey>::iterator lru_position;
    };

private:
    std::shared_ptr<generation::GlobalAtlas const> m_atlas;
    TerrainIdentity m_identity;
    mutable std::mutex m_cache_mutex;
    mutable std::unordered_map<TerrainRegionKey, RegionCacheEntry, RegionKeyHash> m_region_cache;
    mutable std::list<TerrainRegionKey> m_region_lru;
};

} // namespace shared
