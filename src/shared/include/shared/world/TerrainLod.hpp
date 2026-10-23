#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string_view>
#include <vector>

namespace shared {

inline constexpr std::string_view TERRAIN_LOD_MODEL = "lod-canonical-r6-height-v2";
inline constexpr uint64_t TERRAIN_LOD_MAX_NODES = 1'050'625U;

enum class TerrainLodLevel : uint8_t { L0, L1, L2, L3 };

struct TerrainLodNodeKey final {
    uint32_t x = 0U;
    uint32_t y = 0U;
    bool operator==(TerrainLodNodeKey const&) const = default;
};

struct TerrainLodSample final {
    double height = 0.0;
    double parent_height = 0.0;
    uint16_t water_height = 0U;
    bool water_present = false;
    Block top_material = Block::Dirt;
    double temperature = 0.0;
    double precipitation = 0.0;
    bool operator==(TerrainLodSample const&) const = default;
};

struct TerrainLodNode final {
    TerrainLodNodeKey key;
    TerrainLodSample sample;
    bool operator==(TerrainLodNode const&) const = default;
};

struct TerrainLodArtifact final {
    TerrainIdentity terrain_identity;
    std::string_view model_identity = TERRAIN_LOD_MODEL;
    TerrainLodLevel level = TerrainLodLevel::L0;
    uint32_t spacing = 0U;
    uint32_t origin_x = 0U;
    uint32_t origin_y = 0U;
    uint32_t extent = 0U;
    uint32_t side = 0U;
    std::vector<TerrainLodNode> nodes;
};

[[nodiscard]] uint32_t terrainLodSpacing(uint32_t atlas_resolution, TerrainLodLevel level);

// L0 covers the complete periodic world. Other levels require aligned, bounded windows.
[[nodiscard]] std::shared_ptr<TerrainLodArtifact const> buildTerrainLod(
    TerrainWorld const& world,
    TerrainLodLevel level,
    uint32_t origin_x = 0U,
    uint32_t origin_y = 0U,
    uint32_t extent = generation::WORLD_SIZE_BLOCKS,
    std::stop_token const& cancellation = {}
);

[[nodiscard]] TerrainLodSample sampleTerrainLod(
    TerrainLodArtifact const& artifact,
    double world_x,
    double world_y
);

} // namespace shared
