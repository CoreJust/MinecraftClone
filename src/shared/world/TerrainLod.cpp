#include <shared/world/TerrainLod.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <unordered_map>
#include <utility>

namespace {

constexpr uint32_t PERIOD = generation::WORLD_SIZE_BLOCKS;

void checkCancellation(std::stop_token const& cancellation)
{
    if (cancellation.stop_requested()) {
        throw generation::GenerationCancelled{};
    }
}

uint32_t wrap(int64_t const coordinate)
{
    return static_cast<uint32_t>((coordinate % PERIOD + PERIOD) % PERIOD);
}

double blend(double const a, double const b, double const fraction)
{
    return a + (b - a) * fraction;
}

size_t atlasIndex(generation::GlobalAtlas const& atlas, uint32_t const x, uint32_t const y)
{
    uint32_t const resolution = atlas.configuration.resolution;
    return static_cast<size_t>(y / (PERIOD / resolution)) * resolution + x / (PERIOD / resolution);
}

using ParentHeightCache = std::unordered_map<uint64_t, uint16_t>;

uint16_t cachedParentHeight(
    shared::TerrainWorld const& world,
    uint32_t const x,
    uint32_t const y,
    ParentHeightCache& cache
)
{
    uint64_t const key = (static_cast<uint64_t>(y) << 32U) | x;
    auto const found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }
    uint16_t const height = world.sampleSurfaceHeight(x, y);
    cache.emplace(key, height);
    return height;
}

double parentHeight(
    shared::TerrainWorld const& world,
    shared::TerrainLodLevel const level,
    shared::TerrainLodNodeKey const& position,
    double const height,
    ParentHeightCache& cache
)
{
    if (level == shared::TerrainLodLevel::L0) {
        return height;
    }
    auto const parent = static_cast<shared::TerrainLodLevel>(static_cast<uint8_t>(level) - 1U);
    uint32_t const spacing = shared::terrainLodSpacing(world.atlas()->configuration.resolution, parent);
    uint32_t const x0 = position.x / spacing * spacing;
    uint32_t const y0 = position.y / spacing * spacing;
    if (position.x == x0 && position.y == y0) {
        return height;
    }
    double const fx = static_cast<double>(position.x - x0) / spacing;
    double const fy = static_cast<double>(position.y - y0) / spacing;
    return blend(
        blend(cachedParentHeight(world, x0, y0, cache),
            cachedParentHeight(world, wrap(x0 + spacing), y0, cache), fx),
        blend(cachedParentHeight(world, x0, wrap(y0 + spacing), cache),
            cachedParentHeight(world, wrap(x0 + spacing), wrap(y0 + spacing), cache), fx), fy
    );
}

shared::Block surfaceMaterial(shared::TerrainLodSample const& sample, uint32_t const sea_level)
{
    shared::TerrainColumn column;
    column.highest_solid_z = static_cast<uint16_t>(std::floor(sample.height));
    column.top_material = sample.temperature <= -2.0 ? shared::Block::Snow : shared::Block::Grass;
    column.ground_material = sample.temperature <= -2.0 ? shared::Block::Ice : shared::Block::Dirt;
    column.ground_first_z = static_cast<uint16_t>(sea_level > 4U ? sea_level - 4U : 1U);
    column.ground_end_z = static_cast<uint16_t>(column.highest_solid_z + 1U);
    return column.materialAt(column.highest_solid_z, sea_level);
}

shared::TerrainLodSample makeSample(
    shared::TerrainWorld const& world,
    shared::TerrainLodLevel const level,
    uint32_t const x,
    uint32_t const y,
    std::stop_token const& cancellation,
    std::unordered_map<uint64_t, shared::SurfaceTile>& surface_cache,
    ParentHeightCache& parent_height_cache
)
{
    auto const& atlas = *world.atlas();
    size_t const index = atlasIndex(atlas, x, y);
    shared::TerrainLodSample sample;
    sample.height = level == shared::TerrainLodLevel::L3 ? 0.0 : world.sampleSurfaceHeight(x, y);
    auto const climate = world.sampleClimate(x, y);
    sample.temperature = climate.temperature_celsius;
    sample.precipitation = climate.precipitation;
    sample.water_present = atlas.ocean_mask[index] || atlas.lake_mask[index] || atlas.river_mask[index];
    sample.water_height = static_cast<uint16_t>(std::clamp(std::floor(atlas.block_sea_level
        + atlas.vertical_scale * (atlas.water_level[index] - atlas.sea_level)), 0.0, 1023.0));
    sample.top_material = surfaceMaterial(sample, atlas.block_sea_level);
    if (level == shared::TerrainLodLevel::L3) {
        uint64_t const key = static_cast<uint64_t>(y / shared::Chunk::SIDE_LENGTH) * PERIOD
            + x / shared::Chunk::SIDE_LENGTH;
        auto found = surface_cache.find(key);
        if (found == surface_cache.end()) {
            found = surface_cache.emplace(key, world.sampleSurface({
            .x = static_cast<int32_t>(x / shared::Chunk::SIDE_LENGTH),
            .y = static_cast<int32_t>(y / shared::Chunk::SIDE_LENGTH), .z = 0,
        }, cancellation)).first;
        }
        auto const& column = found->second.columns[(y % shared::Chunk::SIDE_LENGTH) * shared::Chunk::SIDE_LENGTH
            + x % shared::Chunk::SIDE_LENGTH];
        sample.height = column.highest_solid_z;
        sample.water_height = column.water_height_z;
        sample.water_present = column.water_present;
        sample.top_material = column.materialAt(column.highest_solid_z, atlas.block_sea_level);
        sample.temperature = column.temperature_celsius;
        sample.precipitation = column.precipitation;
    }
    sample.parent_height = parentHeight(
        world, level, shared::TerrainLodNodeKey{.x = x, .y = y}, sample.height, parent_height_cache
    );
    return sample;
}

} // namespace

namespace shared {

uint32_t terrainLodSpacing(uint32_t const atlas_resolution, TerrainLodLevel const level)
{
    if ((atlas_resolution != 256U && atlas_resolution != 512U)
        || static_cast<uint8_t>(level) > static_cast<uint8_t>(TerrainLodLevel::L3)) {
        throw std::invalid_argument{"LOD scaffold requires a 256 or 512 atlas and a registered level"};
    }
    if (level == TerrainLodLevel::L3) {
        return 1U;
    }
    return PERIOD / atlas_resolution / (1U << (2U * static_cast<uint8_t>(level)));
}

std::shared_ptr<TerrainLodArtifact const> buildTerrainLod(
    TerrainWorld const& world,
    TerrainLodLevel const level,
    uint32_t const origin_x,
    uint32_t const origin_y,
    uint32_t const extent,
    std::stop_token const& cancellation
)
{
    checkCancellation(cancellation);
    uint32_t const spacing = terrainLodSpacing(world.atlas()->configuration.resolution, level);
    uint64_t const side = static_cast<uint64_t>(extent) / spacing + 1U;
    if (extent == 0U || extent > PERIOD || origin_x >= PERIOD || origin_y >= PERIOD
        || origin_x % spacing || origin_y % spacing || extent % spacing
        || side * side > TERRAIN_LOD_MAX_NODES
        || (level == TerrainLodLevel::L0 && (origin_x || origin_y || extent != PERIOD))) {
        throw std::invalid_argument{"LOD scaffold window must be aligned, bounded, and L0 must cover the world"};
    }
    auto artifact = std::make_shared<TerrainLodArtifact>();
    artifact->terrain_identity = world.identity();
    artifact->level = level;
    artifact->spacing = spacing;
    artifact->origin_x = origin_x;
    artifact->origin_y = origin_y;
    artifact->extent = extent;
    artifact->side = static_cast<uint32_t>(side);
    artifact->nodes.reserve(static_cast<size_t>(side * side));
    std::unordered_map<uint64_t, SurfaceTile> surface_cache;
    ParentHeightCache parent_height_cache;
    if (level != TerrainLodLevel::L0) {
        auto const parent = static_cast<TerrainLodLevel>(static_cast<uint8_t>(level) - 1U);
        uint64_t const parent_side = extent / terrainLodSpacing(world.atlas()->configuration.resolution, parent) + 3U;
        parent_height_cache.reserve(static_cast<size_t>(parent_side * parent_side));
    }
    for (uint32_t row = 0U; row < side; ++row) {
        checkCancellation(cancellation);
        for (uint32_t col = 0U; col < side; ++col) {
            uint32_t const x = wrap(origin_x + col * spacing);
            uint32_t const y = wrap(origin_y + row * spacing);
            artifact->nodes.push_back({.key = {.x = x, .y = y},
                .sample = makeSample(world, level, x, y, cancellation, surface_cache, parent_height_cache)});
        }
    }
    checkCancellation(cancellation);
    return artifact;
}

TerrainLodSample sampleTerrainLod(TerrainLodArtifact const& artifact, double world_x, double world_y)
{
    if (!std::isfinite(world_x) || !std::isfinite(world_y) || artifact.spacing == 0U || artifact.side < 2U
        || artifact.nodes.size() != static_cast<uint64_t>(artifact.side) * artifact.side) {
        throw std::invalid_argument{"invalid LOD scaffold artifact or sample position"};
    }
    world_x = std::fmod(std::fmod(world_x - artifact.origin_x, PERIOD) + PERIOD, PERIOD);
    world_y = std::fmod(std::fmod(world_y - artifact.origin_y, PERIOD) + PERIOD, PERIOD);
    if (world_x > artifact.extent || world_y > artifact.extent) {
        throw std::invalid_argument{"LOD scaffold sample is outside its window"};
    }
    uint32_t const col = std::min(static_cast<uint32_t>(world_x / artifact.spacing), artifact.side - 2U);
    uint32_t const row = std::min(static_cast<uint32_t>(world_y / artifact.spacing), artifact.side - 2U);
    double const fx = world_x / artifact.spacing - col;
    double const fy = world_y / artifact.spacing - row;
    auto const& a = artifact.nodes[row * artifact.side + col].sample;
    auto const& b = artifact.nodes[row * artifact.side + col + 1U].sample;
    auto const& c = artifact.nodes[(row + 1U) * artifact.side + col].sample;
    auto const& d = artifact.nodes[(row + 1U) * artifact.side + col + 1U].sample;
    TerrainLodSample result = artifact.nodes[(row + (fy >= 0.5)) * artifact.side + col + (fx >= 0.5)].sample;
    result.height = blend(blend(a.height, b.height, fx), blend(c.height, d.height, fx), fy);
    result.parent_height = blend(blend(a.parent_height, b.parent_height, fx),
        blend(c.parent_height, d.parent_height, fx), fy);
    result.temperature = blend(blend(a.temperature, b.temperature, fx), blend(c.temperature, d.temperature, fx), fy);
    result.precipitation = blend(blend(a.precipitation, b.precipitation, fx),
        blend(c.precipitation, d.precipitation, fx), fy);
    result.top_material = surfaceMaterial(result, generation::BLOCK_SEA_LEVEL);
    return result;
}

} // namespace shared
