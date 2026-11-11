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
inline constexpr std::string_view TERRAIN_LOD_MODEL_V9 = "lod-canonical-v9-height-v1";
inline constexpr std::string_view TERRAIN_LOD_MODEL_V21 =
    "lod-native-v21-a3f58ba8feb417556f6ae90e5e9b96b4ca318c169908dc8f30ce87bce61010cc-r1";
inline constexpr std::string_view TERRAIN_LOD_MODEL_V10 =
    "lod-native-v10-f775213330028b165328885c6899ac0bf9c3c0c9a93ff6e0aca591554dec6e59-r1";
inline constexpr uint64_t TERRAIN_LOD_MAX_NODES = 1'050'625U;

[[nodiscard]]
constexpr std::string_view terrainLodModel(generation::TerrainModelTag const model) noexcept
{
    switch (model) {
    case generation::TerrainModelTag::V9:
        return TERRAIN_LOD_MODEL_V9;
    case generation::TerrainModelTag::V21:
        return TERRAIN_LOD_MODEL_V21;
    case generation::TerrainModelTag::V10:
        return TERRAIN_LOD_MODEL_V10;
    case generation::TerrainModelTag::R6:
        return TERRAIN_LOD_MODEL;
    }
    return {};
}

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
    double source_l0_elevation_m = 0.0;
    uint8_t source_biome_id = 255U;
    bool source_native_coast_adjacent = false;
    bool source_native_ocean = false;
    bool v21_surface = false;
    bool v10_surface = false;
    double land_snow_water_equivalent_m = 0.0;
    double glacier_thickness_m = 0.0;
    double sea_ice_thickness_m = 0.0;
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

[[nodiscard]] Block terrainLodSurfaceMaterial(TerrainLodSample const& sample, uint32_t sea_level);

[[nodiscard]] uint32_t terrainLodSpacing(
    uint32_t atlas_resolution,
    TerrainLodLevel level,
    generation::TerrainModelTag model = generation::TerrainModelTag::R6
);

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
