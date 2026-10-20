#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr double WORLD_SIZE = static_cast<double>(generation::WORLD_SIZE_BLOCKS);
constexpr double DOUBLE_2_POW_53 = 9'007'199'254'740'992.0;
constexpr uint64_t HASH_X = 0x9E37'79B1'85EB'CA87ULL;
constexpr uint64_t HASH_Y = 0xC2B2'AE3D'27D4'EB4FULL;
constexpr uint64_t HASH_LEVEL = 0x1656'67B1'9E37'79F9ULL;
constexpr uint64_t HASH_MIX_1 = 0xBF58'476D'1CE4'E5B9ULL;
constexpr uint64_t HASH_MIX_2 = 0x94D0'49BB'1331'11EBULL;

[[nodiscard]]
uint32_t regionLevelIndex(shared::TerrainRegionKey const key)
{
    auto const target = std::find(
        shared::TERRAIN_REGION_EXTENTS.begin(),
        shared::TERRAIN_REGION_EXTENTS.end(),
        key.extent
    );
    if (target == shared::TERRAIN_REGION_EXTENTS.end()
        || key.origin_x >= generation::WORLD_SIZE_BLOCKS
        || key.origin_y >= generation::WORLD_SIZE_BLOCKS
        || key.origin_x % key.extent != 0U
        || key.origin_y % key.extent != 0U
        || static_cast<uint64_t>(key.origin_x) + key.extent > generation::WORLD_SIZE_BLOCKS
        || static_cast<uint64_t>(key.origin_y) + key.extent > generation::WORLD_SIZE_BLOCKS) {
        throw std::invalid_argument{ "terrain region key is unaligned, out of bounds, or has an unsupported extent" };
    }
    return static_cast<uint32_t>(std::distance(shared::TERRAIN_REGION_EXTENTS.begin(), target));
}

void throwIfCancelled(std::stop_token const cancellation)
{
    if (cancellation.stop_requested()) {
        throw generation::GenerationCancelled{};
    }
}

constexpr std::array<double, 5> DETAIL_AMPLITUDES_KM{
    0.18,
    0.09,
    0.045,
    0.022,
    0.011,
};
constexpr uint32_t FINAL_SURFACE_DETAIL_EXTENT = 16U;
constexpr uint32_t FINAL_SURFACE_DETAIL_PARENT_EXTENT = 64U;
constexpr double FINAL_SURFACE_DETAIL_AMPLITUDE_KM = 0.044;

struct FieldCoordinate final {
    uint32_t x0;
    uint32_t x1;
    uint32_t y0;
    uint32_t y1;
    double fx;
    double fy;
};

struct FieldValues final {
    double elevation;
    double temperature;
    double precipitation;
    double water_level;
    double lake_mask;
    double river_mask;
};

[[nodiscard]]
double wrapCoordinate(double coordinate) noexcept
{
    double wrapped = std::fmod(coordinate, WORLD_SIZE);
    if (wrapped < 0.0) {
        wrapped += WORLD_SIZE;
    }
    return wrapped;
}

[[nodiscard]]
FieldCoordinate fieldCoordinate(
    generation::GlobalAtlas const& atlas,
    double world_x,
    double world_y
) noexcept
{
    uint32_t const resolution = atlas.configuration.resolution;
    double const sx = wrapCoordinate(world_x) * resolution / WORLD_SIZE;
    double const sy = wrapCoordinate(world_y) * resolution / WORLD_SIZE;
    uint32_t const floor_x = static_cast<uint32_t>(std::floor(sx));
    uint32_t const floor_y = static_cast<uint32_t>(std::floor(sy));
    uint32_t const x0 = floor_x % resolution;
    uint32_t const y0 = floor_y % resolution;
    return FieldCoordinate{
        .x0 = x0,
        .x1 = (x0 + 1U) % resolution,
        .y0 = y0,
        .y1 = (y0 + 1U) % resolution,
        .fx = sx - floor_x,
        .fy = sy - floor_y,
    };
}

template <typename TValue>
[[nodiscard]]
double bilinear(
    std::vector<TValue> const& field,
    uint32_t const resolution,
    FieldCoordinate const coordinate
) noexcept
{
    auto at = [&field, resolution](uint32_t const x, uint32_t const y) {
        return static_cast<double>(field[static_cast<size_t>(y) * resolution + x]);
    };

    double const one_minus_x = 1.0 - coordinate.fx;
    double const one_minus_y = 1.0 - coordinate.fy;
    volatile double weight_00 = one_minus_x * one_minus_y;
    volatile double value_00 = weight_00 * at(coordinate.x0, coordinate.y0);
    volatile double weight_10 = coordinate.fx * one_minus_y;
    volatile double value_10 = weight_10 * at(coordinate.x1, coordinate.y0);
    volatile double weight_01 = one_minus_x * coordinate.fy;
    volatile double value_01 = weight_01 * at(coordinate.x0, coordinate.y1);
    volatile double weight_11 = coordinate.fx * coordinate.fy;
    volatile double value_11 = weight_11 * at(coordinate.x1, coordinate.y1);
    volatile double sum_0 = value_00 + value_10;
    volatile double sum_1 = sum_0 + value_01;
    volatile double result = sum_1 + value_11;
    return result;
}

template <typename TValue>
[[nodiscard]]
double fieldValue(
    std::vector<TValue> const& field,
    generation::GlobalAtlas const& atlas,
    double world_x,
    double world_y
) noexcept
{
    return bilinear(
        field,
        atlas.configuration.resolution,
        fieldCoordinate(atlas, world_x, world_y)
    );
}

[[nodiscard]]
FieldValues fieldsAt(
    generation::GlobalAtlas const& atlas,
    double world_x,
    double world_y
) noexcept
{
    FieldCoordinate const coordinate = fieldCoordinate(atlas, world_x, world_y);
    uint32_t const resolution = atlas.configuration.resolution;
    return FieldValues{
        .elevation = bilinear(atlas.elevation, resolution, coordinate),
        .temperature = bilinear(atlas.temperature, resolution, coordinate),
        .precipitation = bilinear(atlas.precipitation, resolution, coordinate),
        .water_level = bilinear(atlas.water_level, resolution, coordinate),
        .lake_mask = bilinear(atlas.lake_mask, resolution, coordinate),
        .river_mask = bilinear(atlas.river_mask, resolution, coordinate),
    };
}

[[nodiscard]]
uint8_t categoricalMaskAt(
    std::vector<uint8_t> const& field,
    generation::GlobalAtlas const& atlas,
    double world_x,
    double world_y
) noexcept
{
    FieldCoordinate const coordinate = fieldCoordinate(atlas, world_x, world_y);
    uint32_t const resolution = atlas.configuration.resolution;
    return field[static_cast<size_t>(coordinate.y0) * resolution + coordinate.x0];
}

[[nodiscard]]
uint64_t hashNoise(uint64_t seed, int64_t ix, int64_t iy, uint32_t const level) noexcept
{
    int64_t const period = static_cast<int64_t>(generation::WORLD_SIZE_BLOCKS / level);
    ix %= period;
    iy %= period;
    if (ix < 0) {
        ix += period;
    }
    if (iy < 0) {
        iy += period;
    }

    uint64_t bits = seed
        ^ (static_cast<uint64_t>(ix) * HASH_X)
        ^ (static_cast<uint64_t>(iy) * HASH_Y)
        ^ (static_cast<uint64_t>(level) * HASH_LEVEL);
    bits ^= bits >> 30U;
    bits *= HASH_MIX_1;
    bits ^= bits >> 27U;
    bits *= HASH_MIX_2;
    bits ^= bits >> 31U;
    return bits;
}

[[nodiscard]]
double latticeNoise(uint64_t const seed, int64_t const ix, int64_t const iy, uint32_t const level) noexcept
{
    return static_cast<double>(hashNoise(seed, ix, iy, level) >> 11U) * (2.0 / DOUBLE_2_POW_53) - 1.0;
}

[[nodiscard]]
double relaxedNode(
    generation::GlobalAtlas const& atlas,
    uint64_t const seed,
    int64_t const ix,
    int64_t const iy,
    uint32_t const level
) noexcept
{
    double const x = static_cast<double>(ix) * level;
    double const y = static_cast<double>(iy) * level;
    double const cell = WORLD_SIZE / atlas.configuration.resolution;
    double const dx = (
        fieldValue(atlas.elevation, atlas, x + cell, y)
        - fieldValue(atlas.elevation, atlas, x - cell, y)
    ) / (2.0 * cell);
    double const dy = (
        fieldValue(atlas.elevation, atlas, x, y + cell)
        - fieldValue(atlas.elevation, atlas, x, y - cell)
    ) / (2.0 * cell);
    double const rain = std::max(fieldValue(atlas.precipitation, atlas, x, y), 0.0);
    double const diffusivity = 0.15 + 0.18 * std::tanh(rain);
    double const east = std::tanh(-dx * 2'000.0);
    double const south = std::tanh(-dy * 2'000.0);

    double const center = latticeNoise(seed, ix, iy, level);
    double const east_node = latticeNoise(seed, ix + 1, iy, level);
    double const west_node = latticeNoise(seed, ix - 1, iy, level);
    double const south_node = latticeNoise(seed, ix, iy + 1, level);
    double const north_node = latticeNoise(seed, ix, iy - 1, level);
    double const weight_east = 1.0 + east;
    double const weight_west = 1.0 - east;
    double const weight_south = 1.0 + south;
    double const weight_north = 1.0 - south;
    double const neighbor = (
        weight_east * east_node
        + weight_west * west_node
        + weight_south * south_node
        + weight_north * north_node
    ) * 0.25;
    return center + diffusivity * (neighbor - center);
}

[[nodiscard]]
double clip01(double value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]]
double detailInfluence(
    generation::GlobalAtlas const& atlas,
    FieldValues const& fields,
    double world_x,
    double world_y
) noexcept
{
    double const cell = WORLD_SIZE / atlas.configuration.resolution;
    double const dx = (
        fieldValue(atlas.elevation, atlas, world_x + cell, world_y)
        - fieldValue(atlas.elevation, atlas, world_x - cell, world_y)
    ) / (2.0 * cell);
    double const dy = (
        fieldValue(atlas.elevation, atlas, world_x, world_y + cell)
        - fieldValue(atlas.elevation, atlas, world_x, world_y - cell)
    ) / (2.0 * cell);
    double const slope = std::hypot(dx, dy);
    double const rainfall = std::max(fields.precipitation, 0.0);
    double const roughness = 0.35 + 0.65 * std::tanh(2'500.0 * slope);
    double const coldness = clip01((-fields.temperature - 5.0) / 30.0);
    double const climate_factor = 1.0 - 0.18 * std::tanh(rainfall) + 0.08 * coldness;
    double const lake = clip01(fields.lake_mask);
    double const river = clip01(fields.river_mask);
    double const corridor = (1.0 - lake) * (1.0 - river) * (1.0 - river);
    double const ocean_factor = fields.elevation <= static_cast<double>(atlas.sea_level) ? 0.4 : 1.0;
    return roughness * climate_factor * corridor * ocean_factor;
}

[[nodiscard]]
double levelContribution(
    generation::GlobalAtlas const& atlas,
    FieldValues const& fields,
    uint64_t const seed,
    double world_x,
    double world_y,
    uint32_t const level_index,
    double const previous_contribution
) noexcept
{
    uint32_t const level = shared::TERRAIN_REGION_EXTENTS[level_index];
    double const ux = world_x / level;
    double const uy = world_y / level;
    int64_t const ix = static_cast<int64_t>(std::floor(ux));
    int64_t const iy = static_cast<int64_t>(std::floor(uy));
    double fx = ux - static_cast<double>(ix);
    double fy = uy - static_cast<double>(iy);
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);

    double const a = relaxedNode(atlas, seed, ix, iy, level);
    double const b = relaxedNode(atlas, seed, ix + 1, iy, level);
    double const c = relaxedNode(atlas, seed, ix, iy + 1, level);
    double const d = relaxedNode(atlas, seed, ix + 1, iy + 1, level);
    double const node_value = (
        (1.0 - fx) * (1.0 - fy) * a
        + fx * (1.0 - fy) * b
        + (1.0 - fx) * fy * c
        + fx * fy * d
    );

    double const amplitude = DETAIL_AMPLITUDES_KM[level_index];
    double ancestry = 0.75 + 0.25 * std::tanh(std::abs(previous_contribution) / std::max(amplitude, 1e-12));
    if (level_index != 0U) {
        uint32_t const parent_level = shared::TERRAIN_REGION_EXTENTS[level_index - 1U];
        double const parent_x = std::fmod(world_x / parent_level, 1.0);
        double const parent_y = std::fmod(world_y / parent_level, 1.0);
        double const cosine_product = std::cos(std::numbers::pi * parent_x)
            * std::cos(std::numbers::pi * parent_y);
        ancestry *= 1.0 - cosine_product * cosine_product;
    }
    return amplitude * detailInfluence(atlas, fields, world_x, world_y) * ancestry * node_value;
}

[[nodiscard]]
double finalSurfaceDetailContribution(
    generation::GlobalAtlas const& atlas,
    FieldValues const& fields,
    uint64_t const seed,
    double const world_x,
    double const world_y,
    double const previous_contribution
) noexcept
{
    constexpr uint32_t LEVEL = FINAL_SURFACE_DETAIL_EXTENT;
    double const ux = world_x / LEVEL;
    double const uy = world_y / LEVEL;
    int64_t const ix = static_cast<int64_t>(std::floor(ux));
    int64_t const iy = static_cast<int64_t>(std::floor(uy));
    double fx = ux - static_cast<double>(ix);
    double fy = uy - static_cast<double>(iy);
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);

    double const a = relaxedNode(atlas, seed, ix, iy, LEVEL);
    double const b = relaxedNode(atlas, seed, ix + 1, iy, LEVEL);
    double const c = relaxedNode(atlas, seed, ix, iy + 1, LEVEL);
    double const d = relaxedNode(atlas, seed, ix + 1, iy + 1, LEVEL);
    double const node_value = (
        (1.0 - fx) * (1.0 - fy) * a
        + fx * (1.0 - fy) * b
        + (1.0 - fx) * fy * c
        + fx * fy * d
    );

    double ancestry = 0.75 + 0.25 * std::tanh(
        std::abs(previous_contribution)
            / std::max(FINAL_SURFACE_DETAIL_AMPLITUDE_KM, 1e-12)
    );
    double const parent_x = std::fmod(world_x / FINAL_SURFACE_DETAIL_PARENT_EXTENT, 1.0);
    double const parent_y = std::fmod(world_y / FINAL_SURFACE_DETAIL_PARENT_EXTENT, 1.0);
    double const cosine_product = std::cos(std::numbers::pi * parent_x)
        * std::cos(std::numbers::pi * parent_y);
    ancestry *= 1.0 - cosine_product * cosine_product;
    return FINAL_SURFACE_DETAIL_AMPLITUDE_KM
        * detailInfluence(atlas, fields, world_x, world_y)
        * ancestry
        * node_value;
}

[[nodiscard]]
double finalSurfaceElevation(
    generation::GlobalAtlas const& atlas,
    FieldValues const& fields,
    uint64_t const seed,
    double const world_x,
    double const world_y,
    double const accumulated_detail,
    double const previous_contribution
) noexcept
{
    double const contribution = finalSurfaceDetailContribution(
        atlas,
        fields,
        seed,
        world_x,
        world_y,
        previous_contribution
    );
    double const updated_detail = accumulated_detail + contribution;
    double refined_elevation = fields.elevation + updated_detail;
    double const sea_level = static_cast<double>(atlas.sea_level);
    if (fields.elevation > sea_level) {
        refined_elevation = std::max(refined_elevation, sea_level + 1e-6);
    } else if (fields.elevation < sea_level) {
        refined_elevation = std::min(refined_elevation, sea_level - 1e-6);
    } else {
        refined_elevation = sea_level;
    }
    if (fields.lake_mask >= 1.0 || fields.river_mask >= 1.0) {
        refined_elevation = fields.elevation;
    }
    return refined_elevation;
}

[[nodiscard]]
uint16_t mappedHeight(double elevation, generation::GlobalAtlas const& atlas) noexcept
{
    double const raw = static_cast<double>(atlas.block_sea_level)
        + static_cast<double>(atlas.vertical_scale)
            * (elevation - static_cast<double>(atlas.sea_level));
    double const bounded = std::clamp(
        std::floor(raw),
        1.0,
        static_cast<double>(atlas.max_height)
    );
    return static_cast<uint16_t>(bounded);
}

[[nodiscard]]
uint16_t mappedWaterHeight(double water_level, generation::GlobalAtlas const& atlas) noexcept
{
    double const raw = static_cast<double>(atlas.block_sea_level)
        + static_cast<double>(atlas.vertical_scale)
            * (water_level - static_cast<double>(atlas.sea_level));
    double const bounded = std::clamp(
        std::floor(raw),
        0.0,
        static_cast<double>(generation::WORLD_DEPTH_BLOCKS - 1U)
    );
    return static_cast<uint16_t>(bounded);
}

[[nodiscard]]
bool regionAxisPosition(
    double world_coordinate,
    uint32_t origin,
    uint32_t extent,
    double& position
) noexcept
{
    if (!std::isfinite(world_coordinate)) {
        return false;
    }
    position = wrapCoordinate(world_coordinate);
    if (position == 0.0 && origin + extent == generation::WORLD_SIZE_BLOCKS) {
        position = WORLD_SIZE;
    }
    return position >= static_cast<double>(origin)
        && position <= static_cast<double>(origin + extent);
}

void validateAtlas(generation::GlobalAtlas const& atlas)
{
    uint32_t const resolution = atlas.configuration.resolution;
    if (resolution < 2U || static_cast<uint64_t>(resolution) * resolution
            > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument{ "terrain atlas resolution must be a square grid of at least 2x2" };
    }
    size_t const expected = static_cast<size_t>(resolution) * resolution;
    auto requireFloatField = [expected](std::vector<float> const& field, char const* name) {
        if (field.size() != expected
            || !std::all_of(field.begin(), field.end(), [](float const value) { return std::isfinite(value); })) {
            throw std::invalid_argument{ std::string{ "terrain atlas field is missing, malformed, or nonfinite: " } + name };
        }
    };
    auto requireMask = [expected](std::vector<uint8_t> const& field, char const* name) {
        if (field.size() != expected
            || !std::all_of(field.begin(), field.end(), [](uint8_t const value) { return value <= 1U; })) {
            throw std::invalid_argument{ std::string{ "terrain atlas mask is missing or malformed: " } + name };
        }
    };

    requireFloatField(atlas.elevation, "elevation");
    requireFloatField(atlas.temperature, "temperature");
    requireFloatField(atlas.precipitation, "precipitation");
    requireFloatField(atlas.water_level, "water_level");
    requireMask(atlas.lake_mask, "lake_mask");
    requireMask(atlas.river_mask, "river_mask");
    requireMask(atlas.ocean_mask, "ocean_mask");
    if (!std::isfinite(atlas.sea_level)
        || !std::isfinite(atlas.vertical_scale)
        || atlas.vertical_scale <= 0.0
        || atlas.sea_level != 0.0F
        || atlas.block_sea_level != generation::BLOCK_SEA_LEVEL
        || atlas.max_height != generation::MAX_HEIGHT) {
        throw std::invalid_argument{ "terrain atlas vertical mapping is outside the world bounds" };
    }
}

[[nodiscard]]
uint16_t groundFirst(uint16_t const height, uint32_t const sea_level, bool const submerged) noexcept
{
    uint32_t const first = submerged
        ? (sea_level > 4U ? sea_level - 4U : 1U)
        : (height > 4U ? static_cast<uint32_t>(height) - 4U : 1U);
    return static_cast<uint16_t>(std::max(first, 1U));
}

[[nodiscard]]
uint16_t groundEnd(uint16_t const height, uint32_t const sea_level, bool const submerged) noexcept
{
    uint32_t const first = groundFirst(height, sea_level, submerged);
    uint32_t end = submerged ? static_cast<uint32_t>(height) + 1U : height;
    end = std::max(end, first);
    return static_cast<uint16_t>(std::min(end, generation::WORLD_DEPTH_BLOCKS));
}

[[nodiscard]]
bool parseUnsignedField(
    std::string_view const field,
    std::string_view const name,
    uint64_t const minimum,
    uint64_t const maximum,
    uint64_t& value
) noexcept
{
    if (!field.starts_with(name)) {
        return false;
    }
    std::string_view const digits = field.substr(name.size());
    if (digits.empty() || (digits.size() > 1U && digits.front() == '0')) {
        return false;
    }
    uint64_t parsed = 0U;
    auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
    if (error != std::errc{} || end != digits.data() + digits.size()
        || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]]
bool hasCanonicalConfigurationIdentity(
    std::string_view const configuration_identity,
    uint64_t const expected_seed
) noexcept
{
    std::array<std::string_view, 13> fields{};
    std::string_view remaining = configuration_identity;
    for (size_t index = 0U; index < fields.size(); ++index) {
        size_t const separator = remaining.find(';');
        if (index + 1U == fields.size()) {
            if (separator != std::string_view::npos) {
                return false;
            }
            fields[index] = remaining;
        } else {
            if (separator == std::string_view::npos) {
                return false;
            }
            fields[index] = remaining.substr(0U, separator);
            remaining.remove_prefix(separator + 1U);
        }
    }

    uint64_t model_revision = 0U;
    constexpr std::string_view SOURCE_PREFIX = "source-map-sha256=";
    if (fields[0] != "terrain-v2"
        || !parseUnsignedField(
            fields[1],
            "model-revision=",
            generation::MODEL_REVISION,
            generation::MODEL_REVISION,
            model_revision
        )
        || !fields[2].starts_with(SOURCE_PREFIX)
        || fields[2].substr(SOURCE_PREFIX.size()) != generation::MODEL_SOURCE_MAP_SHA256) {
        return false;
    }

    uint64_t seed = 0U;
    uint64_t resolution = 0U;
    uint64_t epochs = 0U;
    uint64_t erosion_steps = 0U;
    uint64_t initial_climate_iterations = 0U;
    uint64_t climate_iterations = 0U;
    uint64_t currents = 0U;
    uint64_t coastal_elaboration = 0U;
    return parseUnsignedField(fields[3], "seed=", 0U, std::numeric_limits<uint64_t>::max(), seed)
        && seed == expected_seed
        && parseUnsignedField(fields[4], "resolution=", 256U, 512U, resolution)
        && (resolution == 256U || resolution == 512U)
        && parseUnsignedField(fields[5], "epochs=", 1U, 12U, epochs)
        && parseUnsignedField(fields[6], "erosion-steps=", 0U, 3U, erosion_steps)
        && parseUnsignedField(
            fields[7],
            "initial-climate-iterations=",
            1U,
            24U,
            initial_climate_iterations
        )
        && parseUnsignedField(fields[8], "climate-iterations=", 1U, 80U, climate_iterations)
        && parseUnsignedField(fields[9], "currents=", 0U, 1U, currents)
        && (fields[10] == "wind-sign=-1" || fields[10] == "wind-sign=1")
        && parseUnsignedField(fields[11], "coastal-elaboration=", 0U, 1U, coastal_elaboration)
        && fields[12] == "shelf-exp=exp64-to-f32-v1";
}

[[nodiscard]]
shared::TerrainColumn makeColumn(
    double elevation,
    double temperature,
    double precipitation,
    double water_level,
    bool water_present,
    generation::GlobalAtlas const& atlas
) noexcept
{
    shared::TerrainColumn column;
    column.highest_solid_z = mappedHeight(elevation, atlas);
    column.water_height_z = mappedWaterHeight(water_level, atlas);
    column.water_present = water_present;
    column.submerged = column.highest_solid_z < atlas.block_sea_level;
    column.geological_elevation_km = elevation;
    column.temperature_celsius = temperature;
    column.precipitation = precipitation;
    column.top_material = column.submerged
        ? shared::Block::Dirt
        : (temperature <= -2.0
            ? shared::Block::Snow
            : (precipitation > 0.20 ? shared::Block::Grass : shared::Block::Dirt));
    column.ground_material = temperature <= -2.0 ? shared::Block::Ice : shared::Block::Dirt;
    column.ground_first_z = groundFirst(column.highest_solid_z, atlas.block_sea_level, column.submerged);
    column.ground_end_z = groundEnd(column.highest_solid_z, atlas.block_sea_level, column.submerged);
    return column;
}

} // namespace

namespace shared {

Block TerrainColumn::materialAt(uint16_t const z, uint32_t const block_sea_level) const noexcept
{
    if (z == 0U) {
        return Block::Bedrock;
    }
    if (z > highest_solid_z) {
        return Block::Air;
    }
    if (highest_solid_z >= block_sea_level && z == highest_solid_z) {
        return top_material;
    }
    if (z >= ground_first_z && z < ground_end_z) {
        return ground_material;
    }
    return Block::Stone;
}

TerrainColumn const* SurfaceTile::columnAt(BlockCoordinate const coordinate) const noexcept
{
    if (!Chunk::isValid(coordinate)) {
        return nullptr;
    }
    return &columns[static_cast<size_t>(coordinate.y) * Chunk::SIDE_LENGTH + coordinate.x];
}

bool isValidTerrainIdentity(TerrainIdentity const& identity) noexcept
{
    return identity.terrain_version == generation::TERRAIN_VERSION
        && identity.revision != 0U
        && identity.configuration_identity.size() <= TERRAIN_CONFIGURATION_IDENTITY_MAX_BYTES
        && !identity.configuration_identity.empty()
        && std::all_of(
            identity.configuration_identity.begin(),
            identity.configuration_identity.end(),
            [](unsigned char const value) { return value >= 0x21U && value <= 0x7eU; }
        )
        && hasCanonicalConfigurationIdentity(identity.configuration_identity, identity.seed);
}

bool isValidTerrainColumn(
    TerrainColumn const& column,
    uint32_t const block_sea_level,
    uint32_t const max_height
) noexcept
{
    if (block_sea_level >= generation::WORLD_DEPTH_BLOCKS
        || max_height == 0U
        || max_height >= generation::WORLD_DEPTH_BLOCKS
        || column.highest_solid_z == 0U
        || column.highest_solid_z > max_height
        || column.water_height_z >= generation::WORLD_DEPTH_BLOCKS
        || column.submerged != (column.highest_solid_z < block_sea_level)
        || !std::isfinite(column.geological_elevation_km)
        || !std::isfinite(column.temperature_celsius)
        || !std::isfinite(column.precipitation)
        || !isSolidBlock(column.top_material)
        || !isSolidBlock(column.ground_material)) {
        return false;
    }

    Block const expected_top = column.submerged
        ? Block::Dirt
        : (column.temperature_celsius <= -2.0
            ? Block::Snow
            : (column.precipitation > 0.20 ? Block::Grass : Block::Dirt));
    Block const expected_ground = column.temperature_celsius <= -2.0 ? Block::Ice : Block::Dirt;
    return column.top_material == expected_top
        && column.ground_material == expected_ground
        && column.ground_first_z == groundFirst(
            column.highest_solid_z,
            block_sea_level,
            column.submerged
        )
        && column.ground_end_z == groundEnd(
            column.highest_solid_z,
            block_sea_level,
            column.submerged
        );
}

bool isValidSurfaceTile(SurfaceTile const& tile) noexcept
{
    if (tile.coordinate.z != 0
        || tile.coordinate.x < 0
        || tile.coordinate.y < 0
        || tile.coordinate.x >= static_cast<int32_t>(generation::WORLD_SIZE_BLOCKS / Chunk::SIDE_LENGTH)
        || tile.coordinate.y >= static_cast<int32_t>(generation::WORLD_SIZE_BLOCKS / Chunk::SIDE_LENGTH)
        || !isValidTerrainIdentity(tile.identity)
        || tile.block_sea_level != generation::BLOCK_SEA_LEVEL
        || tile.max_height != generation::MAX_HEIGHT) {
        return false;
    }
    return std::all_of(tile.columns.begin(), tile.columns.end(), [&tile](TerrainColumn const& column) {
        return isValidTerrainColumn(column, tile.block_sea_level, tile.max_height);
    });
}

RegionalArtifact::Node RegionalArtifact::evaluate(double world_x, double world_y) const
{
    uint32_t index = 0U;
    if (sampleIndex(world_x, world_y, index) && m_node_ready[index]) {
        return m_nodes[index];
    }
    world_x = wrapCoordinate(world_x);
    world_y = wrapCoordinate(world_y);

    Node parent_node;
    FieldValues fields{};
    bool water_present = false;
    double base_elevation = 0.0;
    double accumulated_detail = 0.0;
    double previous_contribution = 0.0;
    if (m_parent) {
        parent_node = m_parent->evaluate(world_x, world_y);
        fields = FieldValues{
            .elevation = parent_node.base_elevation,
            .temperature = parent_node.sample.temperature_celsius,
            .precipitation = parent_node.sample.precipitation,
            .water_level = 0.0,
            .lake_mask = 0.0,
            .river_mask = 0.0,
        };
        base_elevation = parent_node.base_elevation;
        accumulated_detail = parent_node.accumulated_detail;
        previous_contribution = parent_node.last_contribution;
        water_present = parent_node.sample.water_present;
        fields.water_level = fieldValue(m_atlas->water_level, *m_atlas, world_x, world_y);
        fields.lake_mask = fieldValue(m_atlas->lake_mask, *m_atlas, world_x, world_y);
        fields.river_mask = fieldValue(m_atlas->river_mask, *m_atlas, world_x, world_y);
    } else {
        fields = fieldsAt(*m_atlas, world_x, world_y);
        base_elevation = fields.elevation;
        water_present = categoricalMaskAt(m_atlas->ocean_mask, *m_atlas, world_x, world_y) != 0U
            || categoricalMaskAt(m_atlas->lake_mask, *m_atlas, world_x, world_y) != 0U
            || categoricalMaskAt(m_atlas->river_mask, *m_atlas, world_x, world_y) != 0U;
    }

    double const contribution = levelContribution(
        *m_atlas,
        fields,
        m_identity.seed,
        world_x,
        world_y,
        m_level_index,
        previous_contribution
    );
    accumulated_detail += contribution;
    double refined_elevation = base_elevation + accumulated_detail;
    double const sea_level = static_cast<double>(m_atlas->sea_level);
    if (base_elevation > sea_level) {
        refined_elevation = std::max(refined_elevation, sea_level + 1e-6);
    } else if (base_elevation < sea_level) {
        refined_elevation = std::min(refined_elevation, sea_level - 1e-6);
    } else {
        refined_elevation = sea_level;
    }
    if (fields.lake_mask >= 1.0 || fields.river_mask >= 1.0) {
        refined_elevation = base_elevation;
    }

    Node result;
    result.base_elevation = base_elevation;
    result.accumulated_detail = accumulated_detail;
    result.last_contribution = contribution;
    static_cast<TerrainColumn&>(result.sample) = makeColumn(
        refined_elevation,
        fields.temperature,
        fields.precipitation,
        fields.water_level,
        water_present,
        *m_atlas
    );
    result.sample.world_x = world_x;
    result.sample.world_y = world_y;
    return result;
}

bool RegionalArtifact::sampleIndex(
    double const world_x,
    double const world_y,
    uint32_t& index
) const noexcept
{
    double position_x = 0.0;
    double position_y = 0.0;
    if (!regionAxisPosition(world_x, m_key.origin_x, m_key.extent, position_x)
        || !regionAxisPosition(world_y, m_key.origin_y, m_key.extent, position_y)) {
        return false;
    }

    double const spacing = static_cast<double>(m_key.extent) / (TERRAIN_REGION_NODE_SIDE_LENGTH - 1U);
    double const grid_x = (position_x - m_key.origin_x) / spacing;
    double const grid_y = (position_y - m_key.origin_y) / spacing;
    double const floor_x = std::floor(grid_x);
    double const floor_y = std::floor(grid_y);
    if (grid_x != floor_x || grid_y != floor_y
        || floor_x < 0.0 || floor_y < 0.0
        || floor_x >= TERRAIN_REGION_NODE_SIDE_LENGTH
        || floor_y >= TERRAIN_REGION_NODE_SIDE_LENGTH) {
        return false;
    }
    index = static_cast<uint32_t>(floor_y) * TERRAIN_REGION_NODE_SIDE_LENGTH
        + static_cast<uint32_t>(floor_x);
    return true;
}

TerrainWorld::TerrainWorld(
    std::shared_ptr<generation::GlobalAtlas const> atlas,
    uint64_t const revision
)
    : m_atlas(std::move(atlas))
{
    if (!m_atlas) {
        throw std::invalid_argument{ "terrain world requires one immutable global atlas" };
    }
    validateAtlas(*m_atlas);
    m_identity = TerrainIdentity{
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = revision,
        .seed = m_atlas->configuration.seed,
        .configuration_identity = generation::configurationIdentity(m_atlas->configuration),
    };
    if (!isValidTerrainIdentity(m_identity)) {
        throw std::invalid_argument{ "terrain world configuration identity is malformed or too long" };
    }
    m_region_cache.reserve(64U);
}

std::shared_ptr<RegionalArtifact const> TerrainWorld::prepareRegion(
    TerrainRegionKey const key,
    std::stop_token const cancellation
) const
{
    uint32_t const target_index = regionLevelIndex(key);
    throwIfCancelled(cancellation);
    std::shared_ptr<RegionalArtifact const> parent;
    for (uint32_t level_index = 0U; level_index <= target_index; ++level_index) {
        throwIfCancelled(cancellation);
        uint32_t const extent = TERRAIN_REGION_EXTENTS[level_index];
        TerrainRegionKey const level_key{
            .origin_x = (key.origin_x / extent) * extent,
            .origin_y = (key.origin_y / extent) * extent,
            .extent = extent,
        };
        if (auto cached = cachedRegion(level_key)) {
            parent = std::move(cached);
        } else {
            std::shared_ptr<RegionalArtifact const> region = refineRegion(
                level_key,
                std::move(parent),
                cancellation
            );
            throwIfCancelled(cancellation);
            parent = cacheRegion(std::move(region));
        }
    }
    throwIfCancelled(cancellation);
    return parent;
}

std::shared_ptr<RegionalArtifact const> TerrainWorld::refineRegion(
    TerrainRegionKey const key,
    std::shared_ptr<RegionalArtifact const> exact_parent,
    std::stop_token const cancellation
) const
{
    uint32_t const level_index = regionLevelIndex(key);
    if (level_index == 0U) {
        if (exact_parent != nullptr) {
            throw std::invalid_argument{ "root terrain region must not have a parent" };
        }
    } else {
        uint32_t const parent_extent = TERRAIN_REGION_EXTENTS[level_index - 1U];
        uint32_t const expected_parent_x = (key.origin_x / parent_extent) * parent_extent;
        uint32_t const expected_parent_y = (key.origin_y / parent_extent) * parent_extent;
        if (exact_parent == nullptr
            || exact_parent->m_level_index != level_index - 1U
            || exact_parent->m_key.origin_x != expected_parent_x
            || exact_parent->m_key.origin_y != expected_parent_y
            || exact_parent->m_key.extent != parent_extent
            || exact_parent->m_atlas.get() != m_atlas.get()
            || exact_parent->m_identity != m_identity) {
            throw std::invalid_argument{ "terrain refinement requires its exact immediately coarser parent" };
        }
    }
    throwIfCancelled(cancellation);
    return buildRegion(key, level_index, std::move(exact_parent), cancellation);
}

RegionalSample TerrainWorld::sampleRegion(
    RegionalArtifact const& region,
    double const world_x,
    double const world_y,
    std::stop_token const cancellation
) const
{
    double position_x = 0.0;
    double position_y = 0.0;
    if (region.m_atlas.get() != m_atlas.get()
        || region.m_identity != m_identity
        || !regionAxisPosition(world_x, region.m_key.origin_x, region.m_key.extent, position_x)
        || !regionAxisPosition(world_y, region.m_key.origin_y, region.m_key.extent, position_y)) {
        throw std::invalid_argument{ "terrain sample is outside its exact regional dependency" };
    }
    throwIfCancelled(cancellation);
    return region.evaluate(world_x, world_y).sample;
}

SurfaceTile TerrainWorld::sampleSurface(
    RegionalArtifact const& leaf,
    ChunkCoordinate const coordinate,
    std::stop_token const cancellation
) const
{
    if (leaf.m_atlas.get() != m_atlas.get()
        || leaf.m_identity != m_identity
        || leaf.m_key.extent != TERRAIN_REGION_EXTENTS.back()
        || coordinate.z != 0
        || coordinate.x < 0
        || coordinate.y < 0
        || coordinate.x >= static_cast<int32_t>(generation::WORLD_SIZE_BLOCKS / Chunk::SIDE_LENGTH)
        || coordinate.y >= static_cast<int32_t>(generation::WORLD_SIZE_BLOCKS / Chunk::SIDE_LENGTH)) {
        throw std::invalid_argument{ "surface sampling requires a canonical 64-block leaf and valid XY chunk" };
    }

    uint32_t const world_x = static_cast<uint32_t>(coordinate.x) * Chunk::SIDE_LENGTH;
    uint32_t const world_y = static_cast<uint32_t>(coordinate.y) * Chunk::SIDE_LENGTH;
    uint32_t const expected_x = (world_x / leaf.m_key.extent) * leaf.m_key.extent;
    uint32_t const expected_y = (world_y / leaf.m_key.extent) * leaf.m_key.extent;
    if (leaf.m_key.origin_x != expected_x || leaf.m_key.origin_y != expected_y) {
        throw std::invalid_argument{ "surface chunk does not belong to the provided 64-block leaf" };
    }
    throwIfCancelled(cancellation);

    SurfaceTile tile;
    tile.coordinate = coordinate;
    tile.block_sea_level = m_atlas->block_sea_level;
    tile.max_height = m_atlas->max_height;
    tile.identity = m_identity;
    for (uint8_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
        throwIfCancelled(cancellation);
        for (uint8_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
            uint32_t const index = static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x;
            double const sample_x = static_cast<double>(world_x + x);
            double const sample_y = static_cast<double>(world_y + y);
            RegionalArtifact::Node const node = leaf.evaluate(sample_x, sample_y);
            FieldValues fields{
                .elevation = node.base_elevation,
                .temperature = node.sample.temperature_celsius,
                .precipitation = node.sample.precipitation,
                .water_level = fieldValue(m_atlas->water_level, *m_atlas, sample_x, sample_y),
                .lake_mask = fieldValue(m_atlas->lake_mask, *m_atlas, sample_x, sample_y),
                .river_mask = fieldValue(m_atlas->river_mask, *m_atlas, sample_x, sample_y),
            };
            double const refined_elevation = finalSurfaceElevation(
                *m_atlas,
                fields,
                m_identity.seed,
                sample_x,
                sample_y,
                node.accumulated_detail,
                node.last_contribution
            );
            tile.columns[index] = makeColumn(
                refined_elevation,
                fields.temperature,
                fields.precipitation,
                fields.water_level,
                node.sample.water_present,
                *m_atlas
            );
        }
    }
    throwIfCancelled(cancellation);
    if (!isValidSurfaceTile(tile)) {
        throw std::logic_error{ "canonical terrain sampler produced an invalid surface tile" };
    }
    return tile;
}

SurfaceTile TerrainWorld::sampleSurface(
    ChunkCoordinate const coordinate,
    std::stop_token const cancellation
) const
{
    if (coordinate.z != 0
        || coordinate.x < 0
        || coordinate.y < 0
        || coordinate.x >= static_cast<int32_t>(generation::WORLD_SIZE_BLOCKS / Chunk::SIDE_LENGTH)
        || coordinate.y >= static_cast<int32_t>(generation::WORLD_SIZE_BLOCKS / Chunk::SIDE_LENGTH)) {
        throw std::invalid_argument{ "surface sampling requires a valid XY chunk coordinate" };
    }
    uint32_t const world_x = static_cast<uint32_t>(coordinate.x) * Chunk::SIDE_LENGTH;
    uint32_t const world_y = static_cast<uint32_t>(coordinate.y) * Chunk::SIDE_LENGTH;
    TerrainRegionKey const key{
        .origin_x = (world_x / TERRAIN_REGION_EXTENTS.back()) * TERRAIN_REGION_EXTENTS.back(),
        .origin_y = (world_y / TERRAIN_REGION_EXTENTS.back()) * TERRAIN_REGION_EXTENTS.back(),
        .extent = TERRAIN_REGION_EXTENTS.back(),
    };
    std::shared_ptr<RegionalArtifact const> const region = prepareRegion(key, cancellation);
    return sampleSurface(*region, coordinate, cancellation);
}

Chunk TerrainWorld::materializeChunk(
    SurfaceTile const& tile,
    ChunkCoordinate const coordinate,
    std::stop_token const cancellation
) const
{
    if (!isValidSurfaceTile(tile)
        || tile.identity != m_identity
        || tile.block_sea_level != m_atlas->block_sea_level
        || tile.max_height != m_atlas->max_height
        || coordinate.x != tile.coordinate.x
        || coordinate.y != tile.coordinate.y
        || coordinate.z < 0
        || coordinate.z >= static_cast<int32_t>(generation::WORLD_DEPTH_BLOCKS / Chunk::SIDE_LENGTH)) {
        throw std::invalid_argument{ "chunk materialization requires a matching canonical surface tile and valid Z" };
    }
    throwIfCancelled(cancellation);

    Chunk::Blocks blocks;
    uint32_t const first_world_z = static_cast<uint32_t>(coordinate.z) * Chunk::SIDE_LENGTH;
    uint32_t const last_world_z = first_world_z + Chunk::SIDE_LENGTH - 1U;
    bool is_uniform_air = true;
    bool is_uniform_stone = first_world_z > 0U
        && last_world_z < generation::WORLD_DEPTH_BLOCKS;
    for (uint32_t column_index = 0U;
        column_index < Chunk::FACE_BLOCK_COUNT && (is_uniform_air || is_uniform_stone);
        ++column_index) {
        if (column_index % Chunk::SIDE_LENGTH == 0U) {
            throwIfCancelled(cancellation);
        }
        TerrainColumn const& column = tile.columns[column_index];
        bool const ground_overlaps = column.ground_first_z <= last_world_z
            && column.ground_end_z > first_world_z;
        bool const land_top_in_chunk = column.highest_solid_z >= tile.block_sea_level
            && column.highest_solid_z >= first_world_z
            && column.highest_solid_z <= last_world_z;
        is_uniform_air = is_uniform_air && first_world_z > column.highest_solid_z;
        is_uniform_stone = is_uniform_stone
            && column.highest_solid_z >= last_world_z
            && !ground_overlaps
            && !land_top_in_chunk;
    }

    if (is_uniform_air) {
        blocks.fill(Block::Air);
        throwIfCancelled(cancellation);
        return Chunk{ coordinate, blocks };
    }
    if (is_uniform_stone) {
        blocks.fill(Block::Stone);
        throwIfCancelled(cancellation);
        return Chunk{ coordinate, blocks };
    }

    for (uint8_t z = 0U; z < Chunk::SIDE_LENGTH; ++z) {
        throwIfCancelled(cancellation);
        uint32_t const world_z = static_cast<uint32_t>(coordinate.z) * Chunk::SIDE_LENGTH + z;
        for (uint8_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
                uint32_t const column_index = static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x;
                uint32_t const block_index = static_cast<uint32_t>(z) * Chunk::FACE_BLOCK_COUNT
                    + static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH
                    + x;
                blocks[block_index] = world_z >= generation::WORLD_DEPTH_BLOCKS
                    ? Block::Air
                    : tile.columns[column_index].materialAt(
                        static_cast<uint16_t>(world_z),
                        tile.block_sea_level
                    );
            }
        }
    }
    throwIfCancelled(cancellation);
    return Chunk{ coordinate, blocks };
}

std::shared_ptr<RegionalArtifact const> TerrainWorld::buildRegion(
    TerrainRegionKey const key,
    uint32_t const level_index,
    std::shared_ptr<RegionalArtifact const> parent,
    std::stop_token const cancellation
) const
{
    throwIfCancelled(cancellation);
    auto region = std::make_shared<RegionalArtifact>();
    region->m_key = key;
    region->m_level_index = level_index;
    region->m_identity = m_identity;
    region->m_atlas = m_atlas;
    region->m_parent = std::move(parent);
    double const spacing = static_cast<double>(key.extent) / (TERRAIN_REGION_NODE_SIDE_LENGTH - 1U);
    for (uint32_t y = 0U; y < TERRAIN_REGION_NODE_SIDE_LENGTH; ++y) {
        throwIfCancelled(cancellation);
        for (uint32_t x = 0U; x < TERRAIN_REGION_NODE_SIDE_LENGTH; ++x) {
            uint32_t const index = y * TERRAIN_REGION_NODE_SIDE_LENGTH + x;
            double const world_x = key.origin_x + x * spacing;
            double const world_y = key.origin_y + y * spacing;
            region->m_nodes[index] = region->evaluate(world_x, world_y);
            region->m_node_ready[index] = true;
        }
    }
    throwIfCancelled(cancellation);
    return region;
}

std::shared_ptr<RegionalArtifact const> TerrainWorld::cachedRegion(TerrainRegionKey const key) const
{
    std::lock_guard lock{ m_cache_mutex };
    auto const found = m_region_cache.find(key);
    if (found == m_region_cache.end()) {
        return nullptr;
    }
    m_region_lru.splice(m_region_lru.begin(), m_region_lru, found->second.lru_position);
    return found->second.region;
}

std::shared_ptr<RegionalArtifact const> TerrainWorld::cacheRegion(
    std::shared_ptr<RegionalArtifact const> region
) const
{
    std::lock_guard lock{ m_cache_mutex };
    TerrainRegionKey const key = region->key();
    auto const found = m_region_cache.find(key);
    if (found != m_region_cache.end()) {
        m_region_lru.splice(m_region_lru.begin(), m_region_lru, found->second.lru_position);
        return found->second.region;
    }

    m_region_lru.push_front(key);
    m_region_cache.emplace(key, RegionCacheEntry{
        .region = region,
        .lru_position = m_region_lru.begin(),
    });
    while (m_region_cache.size() > 64U) {
        TerrainRegionKey const evicted = m_region_lru.back();
        m_region_lru.pop_back();
        m_region_cache.erase(evicted);
    }
    return region;
}

size_t TerrainWorld::RegionKeyHash::operator()(TerrainRegionKey const key) const noexcept
{
    uint64_t bits = (static_cast<uint64_t>(key.extent) << 32U)
        ^ (static_cast<uint64_t>(key.origin_y) << 16U)
        ^ key.origin_x;
    bits ^= bits >> 30U;
    bits *= 0xBF58'476D'1CE4'E5B9ULL;
    bits ^= bits >> 27U;
    bits *= 0x94D0'49BB'1331'11EBULL;
    bits ^= bits >> 31U;
    return static_cast<size_t>(bits);
}

} // namespace shared
