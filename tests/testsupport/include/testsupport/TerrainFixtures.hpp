#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace testsupport {

inline constexpr uint32_t TEST_BLOCK_SEA_LEVEL = generation::BLOCK_SEA_LEVEL;

struct TerrainClimate {
    double temperature_celsius = 0.0;
    double precipitation = 0.25;
};

struct HeightTileRequestMetadata {
    uint64_t revision;
    uint64_t token;
};

[[nodiscard]]
inline shared::TerrainIdentity makeTerrainIdentity(
    uint64_t const seed,
    uint64_t const revision,
    std::string configuration_identity = {}
)
{
    if (configuration_identity.empty()) {
        generation::GenerationConfiguration configuration;
        configuration.seed = seed;
        configuration_identity = generation::configurationIdentity(configuration);
    }
    return shared::TerrainIdentity{
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = revision,
        .seed = seed,
        .configuration_identity = std::move(configuration_identity),
    };
}

[[nodiscard]]
inline std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> makeHeightSamples(
    uint16_t const height = 10U
)
{
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
    heights.fill(height);
    return heights;
}

[[nodiscard]]
inline shared::ServerHeightTileDescriptorMessage makeHeightTileDescriptor(
    shared::WorldConfiguration const& configuration,
    uint64_t const world_revision,
    uint32_t const max_height_tiles,
    uint32_t const max_height_tile_bytes = shared::HEIGHT_TILE_PAYLOAD_BYTES,
    std::string configuration_identity = {}
)
{
    return shared::ServerHeightTileDescriptorMessage{
        .configuration = configuration,
        .world_identity = makeTerrainIdentity(
            configuration.seed,
            world_revision,
            std::move(configuration_identity)
        ),
        .world_revision = world_revision,
        .max_height_tiles = max_height_tiles,
        .max_height_tile_bytes = max_height_tile_bytes,
    };
}

[[nodiscard]]
inline shared::TerrainColumn makeTerrainColumn(
    uint16_t const highest_solid_z,
    uint32_t const block_sea_level = TEST_BLOCK_SEA_LEVEL,
    TerrainClimate const climate = {}
)
{
    bool const submerged = highest_solid_z < block_sea_level;
    uint32_t const ground_first_z = submerged
        ? (block_sea_level > 4U ? block_sea_level - 4U : 1U)
        : (highest_solid_z > 4U ? static_cast<uint32_t>(highest_solid_z) - 4U : 1U);
    uint32_t const ground_end_z = std::max(
        submerged ? static_cast<uint32_t>(highest_solid_z) + 1U : highest_solid_z,
        ground_first_z
    );
    shared::Block const top_material = submerged
        ? shared::Block::Dirt
        : (climate.temperature_celsius <= -2.0 ? shared::Block::Snow : shared::Block::Grass);
    shared::Block const ground_material = climate.temperature_celsius <= -2.0
        ? shared::Block::Ice
        : shared::Block::Dirt;
    return shared::TerrainColumn{
        .highest_solid_z = highest_solid_z,
        .submerged = submerged,
        .temperature_celsius = climate.temperature_celsius,
        .precipitation = climate.precipitation,
        .top_material = top_material,
        .ground_material = ground_material,
        .ground_first_z = static_cast<uint16_t>(ground_first_z),
        .ground_end_z = static_cast<uint16_t>(ground_end_z),
    };
}

[[nodiscard]]
inline shared::SurfaceTile makeSurfaceTile(
    shared::HeightTileKey const key,
    shared::TerrainIdentity identity,
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const& heights
)
{
    shared::SurfaceTile surface{
        .coordinate = { .x = key.x, .y = key.y, .z = 0 },
        .block_sea_level = TEST_BLOCK_SEA_LEVEL,
        .max_height = generation::MAX_HEIGHT,
        .identity = std::move(identity),
    };
    for (uint32_t index = 0U; index < heights.size(); ++index) {
        if (heights[index] < 2U || heights[index] > generation::MAX_HEIGHT + 1U) {
            throw std::invalid_argument{ "terrain fixture height must be one above a valid solid height" };
        }
        surface.columns[index] = makeTerrainColumn(static_cast<uint16_t>(heights[index] - 1U));
    }
    return surface;
}

[[nodiscard]]
inline shared::ServerHeightTileMessage makeServerHeightTile(
    shared::HeightTileKey const key,
    shared::TerrainIdentity identity,
    HeightTileRequestMetadata const request,
    uint16_t const highest_solid_z = 9U,
    double const precipitation = 0.25
)
{
    shared::ServerHeightTileMessage message{
        .key = key,
        .revision = request.revision,
        .token = request.token,
    };
    message.heights.fill(static_cast<uint16_t>(highest_solid_z + 1U));
    message.surface = makeSurfaceTile(key, std::move(identity), message.heights);
    if (precipitation <= 0.20) {
        message.surface.columns.fill(makeTerrainColumn(
            highest_solid_z,
            TEST_BLOCK_SEA_LEVEL,
            TerrainClimate{ .precipitation = precipitation }
        ));
    }
    return message;
}

} // namespace testsupport
