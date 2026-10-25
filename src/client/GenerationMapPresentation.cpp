#include <client/GenerationMapPresentation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace client
{
namespace
{

static constexpr uint32_t MINIMUM_RESOLUTION = 256U;
static constexpr uint32_t MAXIMUM_RESOLUTION = 512U;
static constexpr uint8_t SNOW_CLIMATE_ZONE = 5U;
static constexpr float WATER_ICE_TEMPERATURE_C = -10.0F;

struct MapColorStop final
{
    float value = 0.0F;
    GenerationMapRgba8 color{};
};

struct RasterOptions final
{
    uint32_t output_resolution = 0U;
    float relief_scale = 1.0F;
    float water_progress = 1.0F;
    float climate_progress = 1.0F;
    float ice_progress = 1.0F;
};

struct GenerationMapRasterSource final
{
    uint32_t resolution = 0U;
    std::span<float const> elevation;
    std::span<float const> temperature;
    std::span<float const> precipitation;
    std::span<float const> water_level;
    std::span<uint8_t const> ocean_mask;
    std::span<uint8_t const> lake_mask;
    std::span<uint8_t const> river_mask;
};

struct GenerationMapRasterBacking final
{
    std::vector<float> elevation_km;
    std::vector<float> precipitation_m_year;
    std::vector<float> water_level_km;
};

struct IceGrowthRange final
{
    bool has_target = false;
    float minimum_temperature = 0.0F;
    float maximum_temperature = 0.0F;
};

static constexpr std::array<MapColorStop, 10U> ELEVATION_COLORS{{
    { -4.2F, { 35U, 72U, 94U, 255U } },
    { -2.4F, { 47U, 93U, 113U, 255U } },
    { -0.4F, { 82U, 129U, 141U, 255U } },
    { -0.08F, { 126U, 160U, 159U, 255U } },
    { 0.0F, { 190U, 186U, 154U, 255U } },
    { 0.28F, { 159U, 170U, 131U, 255U } },
    { 0.72F, { 119U, 150U, 116U, 255U } },
    { 1.45F, { 138U, 155U, 132U, 255U } },
    { 2.65F, { 185U, 188U, 177U, 255U } },
    { 4.2F, { 229U, 231U, 224U, 255U } },
}};

static constexpr std::array<MapColorStop, 5U> TEMPERATURE_COLORS{{
    { -20.0F, { 111U, 145U, 166U, 255U } },
    { -2.0F, { 128U, 158U, 166U, 255U } },
    { 10.0F, { 129U, 163U, 130U, 255U } },
    { 20.0F, { 178U, 166U, 119U, 255U } },
    { 34.0F, { 188U, 139U, 116U, 255U } },
}};

static constexpr std::array<MapColorStop, 5U> PRECIPITATION_COLORS{{
    { 0.0F, { 190U, 166U, 123U, 255U } },
    { 0.15F, { 177U, 164U, 122U, 255U } },
    { 0.45F, { 139U, 158U, 121U, 255U } },
    { 1.0F, { 94U, 139U, 109U, 255U } },
    { 2.0F, { 70U, 123U, 105U, 255U } },
}};

[[nodiscard]]
bool hasValidResolution(uint32_t const resolution) noexcept
{
    return resolution == MINIMUM_RESOLUTION || resolution == MAXIMUM_RESOLUTION;
}

[[nodiscard]]
bool hasExactLength(std::span<float const> const field, uint32_t const expected_count) noexcept
{
    return field.size() == static_cast<decltype(field.size())>(expected_count);
}

[[nodiscard]]
bool hasExactLength(std::span<uint8_t const> const field, uint32_t const expected_count) noexcept
{
    return field.size() == static_cast<decltype(field.size())>(expected_count);
}

[[nodiscard]]
bool hasExactLength(std::span<int16_t const> const field, uint32_t const expected_count) noexcept
{
    return field.size() == static_cast<decltype(field.size())>(expected_count);
}

[[nodiscard]]
bool isFiniteField(std::span<float const> const field, uint32_t const expected_count) noexcept
{
    if (!hasExactLength(field, expected_count))
    {
        return false;
    }

    return std::all_of(field.begin(), field.end(), [](float const value) {
        return std::isfinite(value);
    });
}

[[nodiscard]]
bool isMaskField(std::span<uint8_t const> const field, uint32_t const expected_count) noexcept
{
    if (!hasExactLength(field, expected_count))
    {
        return false;
    }

    return std::all_of(field.begin(), field.end(), [](uint8_t const value) {
        return value <= 1U;
    });
}

[[nodiscard]]
bool isDisplayIceMask(std::span<uint8_t const> const surface_kind, uint32_t const expected_count) noexcept
{
    if (surface_kind.empty())
    {
        return true;
    }
    return surface_kind.size() == static_cast<decltype(surface_kind.size())>(expected_count)
        && std::all_of(surface_kind.begin(), surface_kind.end(), [](uint8_t const value) {
            return value <= GENERATION_MAP_ICE_WATER;
        });
}

[[nodiscard]]
bool isFiniteSpan(std::span<float const> const field) noexcept
{
    return std::all_of(field.begin(), field.end(), [](float const value) {
        return std::isfinite(value);
    });
}

[[nodiscard]]
bool isByteMask(std::span<uint8_t const> const field) noexcept
{
    return std::all_of(field.begin(), field.end(), [](uint8_t const value) {
        return value <= 1U;
    });
}

[[nodiscard]]
bool hasValidElevation(GenerationMapRasterSource const& source, uint32_t const count) noexcept
{
    return isFiniteField(source.elevation, count);
}

[[nodiscard]]
bool hasValidHydrology(GenerationMapRasterSource const& source, uint32_t const count) noexcept
{
    return isFiniteField(source.water_level, count)
        && isMaskField(source.ocean_mask, count)
        && isMaskField(source.lake_mask, count)
        && isMaskField(source.river_mask, count);
}

[[nodiscard]]
bool hasValidClimate(GenerationMapRasterSource const& source, uint32_t const count) noexcept
{
    return isFiniteField(source.temperature, count)
        && isFiniteField(source.precipitation, count);
}

[[nodiscard]]
bool hasValidStageFields(
    generation::GenerationStage const stage,
    GenerationMapRasterSource const& source,
    uint32_t const pixel_count,
    bool& uses_hydrology,
    bool& uses_climate
) noexcept
{
    uses_hydrology = false;
    uses_climate = false;
    switch (stage)
    {
    case generation::GenerationStage::Tectonics:
    case generation::GenerationStage::CoastalElaboration:
    case generation::GenerationStage::V9Platec:
    case generation::GenerationStage::V9Terrain:
        return true;
    case generation::GenerationStage::InitialDrainage:
        uses_hydrology = hasValidHydrology(source, pixel_count);
        return uses_hydrology;
    case generation::GenerationStage::ProvisionalClimate:
    case generation::GenerationStage::FinalClimate:
        uses_climate = hasValidClimate(source, pixel_count);
        return uses_climate;
    case generation::GenerationStage::TerrainErosion:
    case generation::GenerationStage::FinalDrainage:
        uses_hydrology = hasValidHydrology(source, pixel_count);
        if (!uses_hydrology)
        {
            return false;
        }
        uses_climate = hasValidClimate(source, pixel_count);
        return true;
    case generation::GenerationStage::V9Climate:
        return isFiniteField(source.temperature, pixel_count) && source.precipitation.empty();
    case generation::GenerationStage::V9Water:
    case generation::GenerationStage::Complete:
        uses_hydrology = hasValidHydrology(source, pixel_count);
        uses_climate = hasValidClimate(source, pixel_count);
        return uses_hydrology && uses_climate;
    default:
        return false;
    }
}

[[nodiscard]]
GenerationMapRasterSource rasterSource(generation::GenerationSnapshot const& snapshot) noexcept
{
    return {
        .resolution = snapshot.resolution,
        .elevation = std::span<float const>{ snapshot.elevation },
        .temperature = std::span<float const>{ snapshot.temperature },
        .precipitation = std::span<float const>{ snapshot.precipitation },
        .water_level = std::span<float const>{ snapshot.water_level },
        .ocean_mask = std::span<uint8_t const>{ snapshot.ocean_mask },
        .lake_mask = std::span<uint8_t const>{ snapshot.lake_mask },
        .river_mask = std::span<uint8_t const>{ snapshot.river_mask },
    };
}

[[nodiscard]]
GenerationMapRasterSource rasterSource(
    generation::GenerationSnapshot const& snapshot,
    GenerationMapRasterBacking& backing
)
{
    if (!snapshot.v9)
    {
        return rasterSource(snapshot);
    }

    generation::V9GenerationSnapshotFields const& fields = *snapshot.v9;
    backing.elevation_km.reserve(fields.source_elevation_m.size());
    for (float const elevation_m : fields.source_elevation_m)
    {
        backing.elevation_km.push_back(elevation_m / 1'000.0F);
    }
    if (!fields.precipitation_mm_year.empty())
    {
        backing.precipitation_m_year.reserve(fields.precipitation_mm_year.size());
        for (float const precipitation_mm_year : fields.precipitation_mm_year)
        {
            backing.precipitation_m_year.push_back(precipitation_mm_year / 1'000.0F);
        }
    }
    if (!fields.water_level_m.empty())
    {
        backing.water_level_km.reserve(fields.water_level_m.size());
        for (float const water_level_m : fields.water_level_m)
        {
            backing.water_level_km.push_back(water_level_m / 1'000.0F);
        }
    }
    return {
        .resolution = snapshot.resolution,
        .elevation = backing.elevation_km,
        .temperature = fields.temperature_celsius,
        .precipitation = backing.precipitation_m_year,
        .water_level = backing.water_level_km,
        .ocean_mask = fields.ocean_mask,
        .lake_mask = fields.lake_mask,
        .river_mask = fields.river_mask,
    };
}

[[nodiscard]]
GenerationMapRasterSource rasterSource(generation::GlobalAtlas const& atlas) noexcept
{
    return {
        .resolution = atlas.configuration.resolution,
        .elevation = std::span<float const>{ atlas.elevation },
        .temperature = std::span<float const>{ atlas.temperature },
        .precipitation = std::span<float const>{ atlas.precipitation },
        .water_level = std::span<float const>{ atlas.water_level },
        .ocean_mask = std::span<uint8_t const>{ atlas.ocean_mask },
        .lake_mask = std::span<uint8_t const>{ atlas.lake_mask },
        .river_mask = std::span<uint8_t const>{ atlas.river_mask },
    };
}

[[nodiscard]]
GenerationMapRgba8 blendColor(
    GenerationMapRgba8 const& base,
    GenerationMapRgba8 const& tint,
    float const strength
) noexcept
{
    GenerationMapRgba8 result{};
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        float const value = static_cast<float>(base[channel])
            + (static_cast<float>(tint[channel]) - static_cast<float>(base[channel])) * strength;
        result[channel] = static_cast<uint8_t>(std::clamp(value, 0.0F, 255.0F));
    }
    result[3U] = 255U;
    return result;
}

[[nodiscard]]
GenerationMapRgba8 rampColor(
    float const value,
    std::span<MapColorStop const> const stops
) noexcept
{
    MapColorStop const& first = stops.front();
    if (value <= first.value)
    {
        return first.color;
    }

    uint32_t const stop_count = static_cast<uint32_t>(stops.size());
    for (uint32_t index = 1U; index < stop_count; ++index)
    {
        MapColorStop const& lower = stops[index - 1U];
        MapColorStop const& upper = stops[index];
        if (value <= upper.value)
        {
            float const amount = (value - lower.value) / (upper.value - lower.value);
            return blendColor(lower.color, upper.color, amount);
        }
    }
    return stops.back().color;
}

[[nodiscard]]
GenerationMapRgba8 elevationColor(float const elevation) noexcept
{
    return rampColor(elevation, ELEVATION_COLORS);
}

[[nodiscard]]
GenerationMapRgba8 waterColor(
    GenerationMapRgba8 const& terrain_color,
    float const elevation,
    float const water_level,
    uint8_t const ocean,
    uint8_t const lake,
    uint8_t const river,
    float const progress
) noexcept
{
    if (ocean != 0U)
    {
        float const depth = std::clamp(water_level - elevation, 0.0F, 2.0F);
        float const strength = (0.16F + depth * 0.07F) * progress;
        return blendColor(terrain_color, { 44U, 97U, 121U, 255U }, strength);
    }
    if (lake != 0U)
    {
        return blendColor(terrain_color, { 76U, 143U, 151U, 255U }, 0.42F * progress);
    }
    if (river != 0U)
    {
        return blendColor(terrain_color, { 83U, 142U, 145U, 255U }, 0.42F * progress);
    }
    return terrain_color;
}

[[nodiscard]]
GenerationMapRgba8 climateColor(
    float const temperature,
    float const precipitation,
    bool const has_temperature,
    bool const has_precipitation
) noexcept
{
    GenerationMapRgba8 const temperature_tint = rampColor(temperature, TEMPERATURE_COLORS);
    GenerationMapRgba8 const precipitation_tint = rampColor(precipitation, PRECIPITATION_COLORS);
    if (has_temperature && has_precipitation)
    {
        return blendColor(temperature_tint, precipitation_tint, 0.5F);
    }
    return has_temperature ? temperature_tint : precipitation_tint;
}

[[nodiscard]]
GenerationMapRgba8 frozenSurfaceColor(
    GenerationMapRgba8 const& color,
    uint8_t const surface_kind,
    float const progress
) noexcept
{
    if (surface_kind == GENERATION_MAP_ICE_SNOW)
    {
        return blendColor(color, { 245U, 248U, 247U, 255U }, 0.76F * progress);
    }
    if (surface_kind == GENERATION_MAP_ICE_WATER)
    {
        return blendColor(color, { 184U, 221U, 232U, 255U }, 0.78F * progress);
    }
    return color;
}

[[nodiscard]]
GenerationMapRgba8 shadedColor(
    GenerationMapRgba8 const& color,
    std::span<float const> const elevation,
    uint32_t const resolution,
    uint32_t const index,
    float const relief_scale
) noexcept
{
    uint32_t const x = index % resolution;
    uint32_t const y = index / resolution;
    uint32_t const west_x = (x + resolution - 1U) % resolution;
    uint32_t const east_x = (x + 1U) % resolution;
    uint32_t const north_y = (y + resolution - 1U) % resolution;
    uint32_t const south_y = (y + 1U) % resolution;
    uint32_t const west_index = y * resolution + west_x;
    uint32_t const east_index = y * resolution + east_x;
    uint32_t const north_index = north_y * resolution + x;
    uint32_t const south_index = south_y * resolution + x;
    float const east_west_slope = (elevation[east_index] - elevation[west_index]) * relief_scale;
    float const south_north_slope = (elevation[south_index] - elevation[north_index]) * relief_scale;
    float const shade = std::clamp(1.0F - 0.035F * (east_west_slope + south_north_slope), 0.88F, 1.12F);

    GenerationMapRgba8 result{};
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        result[channel] = static_cast<uint8_t>(
            std::clamp(static_cast<float>(color[channel]) * shade, 0.0F, 255.0F)
        );
    }
    result[3U] = 255U;
    return result;
}

[[nodiscard]]
float smoothProgress(float const progress) noexcept
{
    float const amount = std::clamp(progress, 0.0F, 1.0F);
    return amount * amount * (3.0F - 2.0F * amount);
}

[[nodiscard]]
IceGrowthRange iceGrowthRange(
    std::span<uint8_t const> const surface_kind,
    std::span<float const> const temperature
) noexcept
{
    IceGrowthRange range;
    if (temperature.empty())
    {
        range.has_target = std::any_of(surface_kind.begin(), surface_kind.end(), [](uint8_t const value) {
            return value != GENERATION_MAP_ICE_NONE;
        });
        return range;
    }
    for (uint32_t index = 0U; index < static_cast<uint32_t>(surface_kind.size()); ++index)
    {
        if (surface_kind[index] == GENERATION_MAP_ICE_NONE)
        {
            continue;
        }
        float const value = temperature[index];
        if (!range.has_target)
        {
            range.minimum_temperature = value;
            range.maximum_temperature = value;
            range.has_target = true;
            continue;
        }
        range.minimum_temperature = std::min(range.minimum_temperature, value);
        range.maximum_temperature = std::max(range.maximum_temperature, value);
    }
    return range;
}

[[nodiscard]]
std::optional<GenerationMapImage> rasterizeGenerationMapFromSource(
    GenerationMapRasterSource const& source,
    bool const uses_hydrology,
    bool const uses_climate,
    std::span<uint8_t const> display_ice_surface_kind,
    std::span<float const> display_ice_temperature,
    RasterOptions const& options
)
{
    if (!hasValidResolution(source.resolution)
        || options.output_resolution == 0U
        || source.resolution % options.output_resolution != 0U)
    {
        return std::nullopt;
    }

    uint32_t const pixel_count = source.resolution * source.resolution;
    if (!hasValidElevation(source, pixel_count)
        || (uses_hydrology && !hasValidHydrology(source, pixel_count))
        || (uses_climate && !hasValidClimate(source, pixel_count))
        || !isDisplayIceMask(display_ice_surface_kind, pixel_count)
        || (!display_ice_temperature.empty()
            && (display_ice_temperature.size() != display_ice_surface_kind.size()
                || !isFiniteSpan(display_ice_temperature))))
    {
        return std::nullopt;
    }

    GenerationMapImage image;
    image.width = options.output_resolution;
    image.height = options.output_resolution;
    image.rgba.resize(options.output_resolution * options.output_resolution);

    uint32_t const source_scale = source.resolution / options.output_resolution;
    IceGrowthRange const ice_range = iceGrowthRange(display_ice_surface_kind, display_ice_temperature);
    float const ice_temperature_limit = ice_range.minimum_temperature
        + (ice_range.maximum_temperature - ice_range.minimum_temperature) * options.ice_progress;

    for (uint32_t y = 0U; y < options.output_resolution; ++y)
    {
        uint32_t const source_y = y * source_scale;
        for (uint32_t x = 0U; x < options.output_resolution; ++x)
        {
            uint32_t const source_x = x * source_scale;
            uint32_t const source_index = source_y * source.resolution + source_x;
            uint32_t const output_index = y * options.output_resolution + x;
            float const elevation = source.elevation[source_index];
            float const displayed_elevation = elevation * options.relief_scale;
            GenerationMapRgba8 color = elevationColor(displayed_elevation);
            if (uses_hydrology)
            {
                color = waterColor(
                    color,
                    elevation,
                    source.water_level[source_index],
                    source.ocean_mask[source_index],
                    source.lake_mask[source_index],
                    source.river_mask[source_index],
                    options.water_progress
                );
            }
            if (uses_climate)
            {
                color = blendColor(
                    color,
                    climateColor(source.temperature[source_index], source.precipitation[source_index], true, true),
                    0.32F * options.climate_progress
                );
            }
            if (ice_range.has_target && !display_ice_surface_kind.empty())
            {
                uint8_t const surface_kind = display_ice_surface_kind[source_index];
                bool const target_is_reached = display_ice_temperature.empty()
                    || display_ice_temperature[source_index] <= ice_temperature_limit;
                if (surface_kind != GENERATION_MAP_ICE_NONE && target_is_reached)
                {
                    color = frozenSurfaceColor(color, surface_kind, 1.0F);
                }
            }
            image.rgba[output_index] = shadedColor(
                color,
                source.elevation,
                source.resolution,
                source_index,
                options.relief_scale
            );
        }
    }

    return image;
}

[[nodiscard]]
std::optional<GenerationMapImage> rasterizeGenerationMapInternal(
    generation::GenerationSnapshot const& snapshot,
    std::span<uint8_t const> display_ice_surface_kind,
    std::span<float const> display_ice_temperature,
    RasterOptions const& options
)
{
    if (snapshot.v9 && !generation::isValidGenerationSnapshot(snapshot))
    {
        return std::nullopt;
    }
    GenerationMapRasterBacking backing;
    GenerationMapRasterSource const source = rasterSource(snapshot, backing);
    if (!hasValidResolution(source.resolution) || options.output_resolution == 0U
        || source.resolution % options.output_resolution != 0U)
    {
        return std::nullopt;
    }

    uint32_t const pixel_count = source.resolution * source.resolution;
    if (!hasValidElevation(source, pixel_count))
    {
        return std::nullopt;
    }

    bool uses_hydrology = false;
    bool uses_climate = false;
    if (!hasValidStageFields(snapshot.stage, source, pixel_count, uses_hydrology, uses_climate))
    {
        return std::nullopt;
    }
    return rasterizeGenerationMapFromSource(
        source,
        uses_hydrology,
        uses_climate,
        display_ice_surface_kind.empty() && snapshot.v9
            ? std::span<uint8_t const>{ snapshot.v9->ice_surface_kind }
            : display_ice_surface_kind,
        display_ice_temperature,
        options
    );
}

struct AnimationPoint final
{
    float x = 0.0F;
    float y = 0.0F;
};

struct AnimationPath final
{
    std::vector<AnimationPoint> points;
    float phase = 0.0F;
};

struct AnimationRainDrop final
{
    uint32_t cell_index = 0U;
    float phase = 0.0F;
};

struct AnimationCell final
{
    float elevation = 0.0F;
    float water_level = 0.0F;
    float temperature = 0.0F;
    float precipitation = 0.0F;
    float moisture = 0.0F;
    float wind_u = 0.0F;
    float wind_v = 0.0F;
    float current_u = 0.0F;
    float current_v = 0.0F;
    float plate_u = 0.0F;
    float plate_v = 0.0F;
    float strain = 0.0F;
    float ice_temperature = 0.0F;
    float wetness = 0.0F;
    uint8_t water_mask = 0U;
    uint8_t ice_kind = GENERATION_MAP_ICE_NONE;
    int16_t plate_index = -1;
};

struct AnimationSourceFields final
{
    uint32_t resolution = 0U;
    std::span<float const> elevation;
    std::span<float const> temperature;
    std::span<float const> precipitation;
    std::span<float const> water_level;
    std::span<float const> moisture;
    std::span<float const> wind_u;
    std::span<float const> wind_v;
    std::span<float const> current_u;
    std::span<float const> current_v;
    std::span<float const> plate_vx;
    std::span<float const> plate_vy;
    std::span<int16_t const> plate_id;
    std::span<uint8_t const> ocean_mask;
    std::span<uint8_t const> lake_mask;
    std::span<uint8_t const> river_mask;
    std::span<uint8_t const> ice_kind;
    std::span<float const> ice_temperature;
    bool hydrology_available = false;
    bool climate_available = false;
    float sea_level = 0.0F;
};

struct PreparedAnimationMap final
{
    uint32_t resolution = GENERATION_MAP_ANIMATION_RESOLUTION;
    std::vector<AnimationCell> cells;
    std::vector<int32_t> nearest_water;
    std::vector<AnimationPath> wind_paths;
    std::vector<AnimationPath> current_paths;
    std::vector<AnimationRainDrop> rain_drops;
    float sea_level = 0.0F;
    float elevation_range = 1.0F;
    float minimum_ice_temperature = 0.0F;
    float maximum_ice_temperature = 0.0F;
    bool has_ice = false;
    bool has_hydrology = false;
    bool has_climate = false;
    bool has_moisture = false;
    bool has_temperature = false;
    bool has_precipitation = false;
    bool has_wind = false;
    bool has_currents = false;
    bool has_motion = false;
};

struct PlateAverage final
{
    double u = 0.0;
    double v = 0.0;
    uint32_t count = 0U;
    uint32_t index = 0U;
};

[[nodiscard]]
uint32_t animationCellIndex(uint32_t const x, uint32_t const y) noexcept
{
    return y * GENERATION_MAP_ANIMATION_RESOLUTION + x;
}

[[nodiscard]]
uint32_t sampleIndex(uint32_t const resolution, uint32_t const stride, uint32_t const x, uint32_t const y) noexcept
{
    return y * stride * resolution + x * stride;
}

[[nodiscard]]
bool validDisplayIceFields(AnimationSourceFields const& fields, uint32_t const pixel_count) noexcept
{
    if (fields.ice_kind.empty())
    {
        return false;
    }
    return fields.ice_kind.size() == static_cast<decltype(fields.ice_kind.size())>(pixel_count)
        && std::all_of(fields.ice_kind.begin(), fields.ice_kind.end(), [](uint8_t const value) {
            return value <= GENERATION_MAP_ICE_WATER;
        })
        && (fields.ice_temperature.empty()
            || (fields.ice_temperature.size() == fields.ice_kind.size() && isFiniteSpan(fields.ice_temperature)));
}

[[nodiscard]]
std::vector<int32_t> nearestWaterSources(std::span<AnimationCell const> const cells)
{
    uint32_t const cell_count = GENERATION_MAP_ANIMATION_RESOLUTION * GENERATION_MAP_ANIMATION_RESOLUTION;
    std::vector<int32_t> nearest(cell_count, -1);
    std::vector<uint32_t> queue;
    queue.reserve(cell_count);
    for (uint32_t index = 0U; index < cell_count; ++index)
    {
        if (cells[index].water_mask == 0U)
        {
            continue;
        }
        nearest[index] = static_cast<int32_t>(index);
        queue.push_back(index);
    }
    for (size_t head = 0U; head < queue.size(); ++head)
    {
        uint32_t const index = queue[head];
        uint32_t const x = index % GENERATION_MAP_ANIMATION_RESOLUTION;
        uint32_t const y = index / GENERATION_MAP_ANIMATION_RESOLUTION;
        std::array<uint32_t, 4U> const neighbors{
            animationCellIndex((x + GENERATION_MAP_ANIMATION_RESOLUTION - 1U) % GENERATION_MAP_ANIMATION_RESOLUTION, y),
            animationCellIndex((x + 1U) % GENERATION_MAP_ANIMATION_RESOLUTION, y),
            animationCellIndex(x, (y + GENERATION_MAP_ANIMATION_RESOLUTION - 1U) % GENERATION_MAP_ANIMATION_RESOLUTION),
            animationCellIndex(x, (y + 1U) % GENERATION_MAP_ANIMATION_RESOLUTION),
        };
        for (uint32_t const next : neighbors)
        {
            if (nearest[next] != -1)
            {
                continue;
            }
            nearest[next] = nearest[index];
            queue.push_back(next);
        }
    }
    return nearest;
}

[[nodiscard]]
std::vector<AnimationPath> makeAnimationPaths(
    PreparedAnimationMap const& data,
    bool const water_only,
    uint32_t const limit
)
{
    std::vector<AnimationPath> paths;
    uint32_t const columns = water_only ? 8U : 4U;
    for (uint32_t row = 0U; row < columns; ++row)
    {
        for (uint32_t column = 0U; column < columns; ++column)
        {
            float x = (static_cast<float>(column) + 0.5F) * static_cast<float>(data.resolution)
                / static_cast<float>(columns);
            float y = (static_cast<float>(row) + 0.5F) * static_cast<float>(data.resolution)
                / static_cast<float>(columns);
            AnimationPath path;
            for (uint32_t step = 0U; step < 32U; ++step)
            {
                if (x < 0.0F || y < 0.0F || x >= static_cast<float>(data.resolution)
                    || y >= static_cast<float>(data.resolution))
                {
                    break;
                }
                uint32_t const source_x = static_cast<uint32_t>(x);
                uint32_t const source_y = static_cast<uint32_t>(y);
                uint32_t const index = animationCellIndex(source_x, source_y);
                AnimationCell const& cell = data.cells[index];
                if (cell.ice_kind != GENERATION_MAP_ICE_NONE
                    || (water_only && (cell.water_mask & 3U) == 0U))
                {
                    break;
                }
                float const u = water_only ? cell.current_u : cell.wind_u;
                float const v = water_only ? cell.current_v : cell.wind_v;
                float const magnitude = std::hypot(u, v);
                if (!std::isfinite(magnitude) || magnitude < 1.0e-8F)
                {
                    break;
                }
                path.points.push_back({ x, y });
                float const next_x = x + u / magnitude * 0.8F;
                float const next_y = y + v / magnitude * 0.8F;
                if (next_x < 0.0F || next_y < 0.0F || next_x >= static_cast<float>(data.resolution)
                    || next_y >= static_cast<float>(data.resolution))
                {
                    break;
                }
                x = next_x;
                y = next_y;
            }
            if (path.points.size() <= 6U)
            {
                continue;
            }
            path.phase = static_cast<float>((column * 23U + row * 41U) % 31U);
            paths.push_back(std::move(path));
        }
    }
    std::sort(paths.begin(), paths.end(), [](AnimationPath const& left, AnimationPath const& right) {
        return left.phase < right.phase;
    });
    if (paths.size() > static_cast<size_t>(limit))
    {
        paths.resize(limit);
    }
    return paths;
}

[[nodiscard]]
std::optional<PreparedAnimationMap> prepareAnimationMap(AnimationSourceFields const& source)
{
    if (!hasValidResolution(source.resolution))
    {
        return std::nullopt;
    }
    uint32_t const pixel_count = source.resolution * source.resolution;
    if (!hasExactLength(source.elevation, pixel_count) || !isFiniteSpan(source.elevation))
    {
        return std::nullopt;
    }

    PreparedAnimationMap data;
    data.sea_level = std::isfinite(source.sea_level) ? source.sea_level : 0.0F;
    data.has_hydrology = source.hydrology_available;
    data.has_temperature = hasExactLength(source.temperature, pixel_count) && isFiniteSpan(source.temperature);
    data.has_precipitation = hasExactLength(source.precipitation, pixel_count)
        && isFiniteSpan(source.precipitation);
    data.has_climate = source.climate_available && (data.has_temperature || data.has_precipitation);
    data.has_moisture = hasExactLength(source.moisture, pixel_count) && isFiniteSpan(source.moisture);
    data.has_wind = hasExactLength(source.wind_u, pixel_count) && hasExactLength(source.wind_v, pixel_count)
        && isFiniteSpan(source.wind_u) && isFiniteSpan(source.wind_v);
    data.has_currents = data.has_hydrology
        && hasExactLength(source.current_u, pixel_count) && hasExactLength(source.current_v, pixel_count)
        && isFiniteSpan(source.current_u) && isFiniteSpan(source.current_v);
    data.has_ice = validDisplayIceFields(source, pixel_count);

    uint32_t const stride = source.resolution / GENERATION_MAP_ANIMATION_RESOLUTION;
    uint32_t const cell_count = GENERATION_MAP_ANIMATION_RESOLUTION * GENERATION_MAP_ANIMATION_RESOLUTION;
    data.cells.resize(cell_count);
    float minimum_elevation = std::numeric_limits<float>::infinity();
    float maximum_elevation = -std::numeric_limits<float>::infinity();
    float maximum_moisture = 0.0F;
    float maximum_precipitation = 0.0F;
    bool has_ice_temperature = false;
    for (uint32_t y = 0U; y < GENERATION_MAP_ANIMATION_RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < GENERATION_MAP_ANIMATION_RESOLUTION; ++x)
        {
            uint32_t const source_index = sampleIndex(source.resolution, stride, x, y);
            AnimationCell& cell = data.cells[animationCellIndex(x, y)];
            cell.elevation = source.elevation[source_index];
            minimum_elevation = std::min(minimum_elevation, cell.elevation);
            maximum_elevation = std::max(maximum_elevation, cell.elevation);
            if (data.has_hydrology)
            {
                cell.water_level = source.water_level[source_index];
                cell.water_mask = static_cast<uint8_t>(
                    (source.ocean_mask[source_index] != 0U ? 1U : 0U)
                    | (source.lake_mask[source_index] != 0U ? 2U : 0U)
                    | (source.river_mask[source_index] != 0U ? 4U : 0U)
                );
            }
            if (data.has_temperature)
            {
                cell.temperature = source.temperature[source_index];
            }
            if (data.has_precipitation)
            {
                cell.precipitation = source.precipitation[source_index];
            }
            if (data.has_moisture)
            {
                cell.moisture = source.moisture[source_index];
                maximum_moisture = std::max(maximum_moisture, cell.moisture);
            }
            if (data.has_precipitation)
            {
                maximum_precipitation = std::max(maximum_precipitation, cell.precipitation);
            }
            if (data.has_wind)
            {
                cell.wind_u = source.wind_u[source_index];
                cell.wind_v = source.wind_v[source_index];
            }
            if (data.has_currents)
            {
                cell.current_u = source.current_u[source_index];
                cell.current_v = source.current_v[source_index];
            }
            if (data.has_ice)
            {
                cell.ice_kind = source.ice_kind[source_index];
                if (!source.ice_temperature.empty())
                {
                    cell.ice_temperature = source.ice_temperature[source_index];
                    if (cell.ice_kind != GENERATION_MAP_ICE_NONE)
                    {
                        if (!has_ice_temperature)
                        {
                            data.minimum_ice_temperature = cell.ice_temperature;
                            data.maximum_ice_temperature = cell.ice_temperature;
                            has_ice_temperature = true;
                        }
                        data.minimum_ice_temperature = std::min(data.minimum_ice_temperature, cell.ice_temperature);
                        data.maximum_ice_temperature = std::max(data.maximum_ice_temperature, cell.ice_temperature);
                    }
                }
            }
        }
    }
    data.elevation_range = std::max(0.001F, maximum_elevation - minimum_elevation);

    if (data.has_ice)
    {
        data.has_ice = std::any_of(data.cells.begin(), data.cells.end(), [](AnimationCell const& cell) {
            return cell.ice_kind != GENERATION_MAP_ICE_NONE;
        });
    }

    for (AnimationCell& cell : data.cells)
    {
        float const moisture = data.has_moisture
            ? cell.moisture / std::max(maximum_moisture, 1.0e-6F) * 0.4F
            : 0.0F;
        float const rainfall = data.has_precipitation
            ? std::log1p(std::max(0.0F, cell.precipitation))
                / std::max(std::log1p(maximum_precipitation), 1.0e-6F) * 0.6F
            : 0.0F;
        cell.wetness = moisture + rainfall;
    }

    if (data.has_hydrology)
    {
        data.nearest_water = nearestWaterSources(data.cells);
    }

    uint32_t plate_count = 0U;
    bool const plate_fields_valid = hasExactLength(source.plate_id, pixel_count)
        && hasExactLength(source.plate_vx, pixel_count) && hasExactLength(source.plate_vy, pixel_count)
        && std::all_of(source.plate_id.begin(), source.plate_id.end(), [](int16_t const value) { return value >= 0; })
        && isFiniteSpan(source.plate_vx) && isFiniteSpan(source.plate_vy);
    if (plate_fields_valid)
    {
        std::map<int16_t, PlateAverage> plates;
        for (uint32_t y = 0U; y < GENERATION_MAP_ANIMATION_RESOLUTION; ++y)
        {
            for (uint32_t x = 0U; x < GENERATION_MAP_ANIMATION_RESOLUTION; ++x)
            {
                uint32_t const source_index = sampleIndex(source.resolution, stride, x, y);
                uint32_t const cell_index = animationCellIndex(x, y);
                int16_t const plate_id = source.plate_id[source_index];
                PlateAverage& plate = plates[plate_id];
                if (plate.count == 0U)
                {
                    plate.index = static_cast<uint32_t>(plates.size() - 1U);
                }
                plate.u += source.plate_vx[source_index];
                plate.v += source.plate_vy[source_index];
                ++plate.count;
                data.cells[cell_index].plate_index = static_cast<int16_t>(plate.index);
            }
        }
        plate_count = static_cast<uint32_t>(plates.size());
        std::vector<std::array<float, 2U>> plate_motion(plate_count);
        float maximum_speed = 0.0F;
        for (auto& [plate_id, plate] : plates)
        {
            static_cast<void>(plate_id);
            plate.u /= static_cast<double>(plate.count);
            plate.v /= static_cast<double>(plate.count);
            maximum_speed = std::max(maximum_speed, static_cast<float>(std::hypot(plate.u, plate.v)));
            plate_motion[plate.index] = {
                static_cast<float>(plate.u),
                static_cast<float>(plate.v),
            };
        }
        if (plate_count <= 64U && maximum_speed > 1.0e-8F)
        {
            data.has_motion = true;
            for (auto& motion : plate_motion)
            {
                motion[0U] /= maximum_speed;
                motion[1U] /= maximum_speed;
            }
            for (AnimationCell& cell : data.cells)
            {
                if (cell.plate_index >= 0)
                {
                    auto const& motion = plate_motion[static_cast<size_t>(cell.plate_index)];
                    cell.plate_u = motion[0U];
                    cell.plate_v = motion[1U];
                }
            }

            std::vector<float> strain(cell_count, 0.0F);
            float maximum_strain = 0.0F;
            for (uint32_t y = 0U; y < GENERATION_MAP_ANIMATION_RESOLUTION; ++y)
            {
                for (uint32_t x = 0U; x < GENERATION_MAP_ANIMATION_RESOLUTION; ++x)
                {
                    uint32_t const index = animationCellIndex(x, y);
                    uint32_t const east = animationCellIndex((x + 1U) % GENERATION_MAP_ANIMATION_RESOLUTION, y);
                    uint32_t const west = animationCellIndex(
                        (x + GENERATION_MAP_ANIMATION_RESOLUTION - 1U) % GENERATION_MAP_ANIMATION_RESOLUTION,
                        y
                    );
                    uint32_t const south = animationCellIndex(x, (y + 1U) % GENERATION_MAP_ANIMATION_RESOLUTION);
                    uint32_t const north = animationCellIndex(
                        x,
                        (y + GENERATION_MAP_ANIMATION_RESOLUTION - 1U) % GENERATION_MAP_ANIMATION_RESOLUTION
                    );
                    auto const sourcePlateId = [&](uint32_t const cell_index) {
                        uint32_t const cell_x = cell_index % GENERATION_MAP_ANIMATION_RESOLUTION;
                        uint32_t const cell_y = cell_index / GENERATION_MAP_ANIMATION_RESOLUTION;
                        return source.plate_id[sampleIndex(source.resolution, stride, cell_x, cell_y)];
                    };
                    AnimationCell const& cell = data.cells[index];
                    float value = 0.0F;
                    for (auto const [neighbor, nx, ny] : std::array<std::array<int32_t, 3U>, 4U>{{
                        { static_cast<int32_t>(east), 1, 0 },
                        { static_cast<int32_t>(west), -1, 0 },
                        { static_cast<int32_t>(south), 0, 1 },
                        { static_cast<int32_t>(north), 0, -1 },
                    }})
                    {
                        uint32_t const neighbor_index = static_cast<uint32_t>(neighbor);
                        if (sourcePlateId(neighbor_index) == sourcePlateId(index))
                        {
                            continue;
                        }
                        AnimationCell const& neighbor_cell = data.cells[neighbor_index];
                        value += (cell.plate_u - neighbor_cell.plate_u) * static_cast<float>(nx)
                            + (cell.plate_v - neighbor_cell.plate_v) * static_cast<float>(ny);
                    }
                    strain[index] = value;
                    maximum_strain = std::max(maximum_strain, std::abs(value));
                }
            }
            for (uint32_t pass = 0U; pass < 3U; ++pass)
            {
                std::vector<float> smoothed(cell_count, 0.0F);
                for (uint32_t y = 0U; y < GENERATION_MAP_ANIMATION_RESOLUTION; ++y)
                {
                    for (uint32_t x = 0U; x < GENERATION_MAP_ANIMATION_RESOLUTION; ++x)
                    {
                        uint32_t const index = animationCellIndex(x, y);
                        uint32_t const west = animationCellIndex(
                            (x + GENERATION_MAP_ANIMATION_RESOLUTION - 1U) % GENERATION_MAP_ANIMATION_RESOLUTION,
                            y
                        );
                        uint32_t const east = animationCellIndex((x + 1U) % GENERATION_MAP_ANIMATION_RESOLUTION, y);
                        uint32_t const north = animationCellIndex(
                            x,
                            (y + GENERATION_MAP_ANIMATION_RESOLUTION - 1U) % GENERATION_MAP_ANIMATION_RESOLUTION
                        );
                        uint32_t const south = animationCellIndex(x, (y + 1U) % GENERATION_MAP_ANIMATION_RESOLUTION);
                        smoothed[index] = (strain[index] * 2.0F + strain[west] + strain[east]
                            + strain[north] + strain[south]) / 6.0F;
                    }
                }
                strain = std::move(smoothed);
            }
            float const denominator = std::max(maximum_strain, 1.0e-8F);
            for (uint32_t index = 0U; index < cell_count; ++index)
            {
                data.cells[index].strain = strain[index] / denominator;
            }
        }
    }

    if (data.has_wind)
    {
        data.wind_paths = makeAnimationPaths(data, false, 16U);
    }
    if (data.has_currents)
    {
        data.current_paths = makeAnimationPaths(data, true, 18U);
    }
    if (data.has_moisture || data.has_climate)
    {
        for (uint32_t y = 4U; y < data.resolution; y += 8U)
        {
            for (uint32_t x = 4U; x < data.resolution; x += 8U)
            {
                uint32_t const index = animationCellIndex(x, y);
                AnimationCell const& cell = data.cells[index];
                if (cell.ice_kind != GENERATION_MAP_ICE_NONE
                    || (data.has_temperature && cell.temperature <= 0.0F))
                {
                    continue;
                }
                if (cell.wetness < 0.22F)
                {
                    continue;
                }
                data.rain_drops.push_back({
                    .cell_index = index,
                    .phase = static_cast<float>((x * 37U + y * 19U) % 97U) / 97.0F,
                });
            }
        }
        std::sort(data.rain_drops.begin(), data.rain_drops.end(), [&data](
            AnimationRainDrop const& left,
            AnimationRainDrop const& right
        ) {
            return data.cells[left.cell_index].wetness > data.cells[right.cell_index].wetness;
        });
        if (data.rain_drops.size() > 32U)
        {
            data.rain_drops.resize(32U);
        }
    }
    return data;
}

[[nodiscard]]
std::optional<PreparedAnimationMap> prepareAnimationMap(
    generation::GlobalAtlas const& atlas,
    GenerationMapDisplayIceMaskHandle const& final_display_ice_mask
)
{
    uint32_t const resolution = atlas.configuration.resolution;
    if (!hasValidResolution(resolution))
    {
        return std::nullopt;
    }
    uint32_t const pixel_count = resolution * resolution;
    bool const water_valid = isFiniteField(std::span<float const>{ atlas.water_level }, pixel_count)
        && isMaskField(std::span<uint8_t const>{ atlas.ocean_mask }, pixel_count)
        && isMaskField(std::span<uint8_t const>{ atlas.lake_mask }, pixel_count)
        && isMaskField(std::span<uint8_t const>{ atlas.river_mask }, pixel_count);
    bool const climate_valid = isFiniteField(std::span<float const>{ atlas.temperature }, pixel_count)
        && isFiniteField(std::span<float const>{ atlas.precipitation }, pixel_count);
    AnimationSourceFields fields{
        .resolution = resolution,
        .elevation = atlas.elevation,
        .temperature = atlas.temperature,
        .precipitation = atlas.precipitation,
        .water_level = atlas.water_level,
        .moisture = atlas.moisture,
        .wind_u = atlas.wind_u,
        .wind_v = atlas.wind_v,
        .current_u = atlas.current_u,
        .current_v = atlas.current_v,
        .plate_vx = atlas.plate_vx,
        .plate_vy = atlas.plate_vy,
        .plate_id = atlas.plate_id,
        .ocean_mask = atlas.ocean_mask,
        .lake_mask = atlas.lake_mask,
        .river_mask = atlas.river_mask,
        .ice_kind = final_display_ice_mask
            ? std::span<uint8_t const>{ final_display_ice_mask->surface_kind }
            : std::span<uint8_t const>{},
        .ice_temperature = final_display_ice_mask
            ? std::span<float const>{ final_display_ice_mask->temperature }
            : std::span<float const>{},
        .hydrology_available = water_valid,
        .climate_available = climate_valid,
        .sea_level = atlas.sea_level,
    };
    return prepareAnimationMap(fields);
}

[[nodiscard]]
std::optional<PreparedAnimationMap> prepareAnimationMap(
    generation::GenerationSnapshot const& snapshot,
    GenerationMapDisplayIceMaskHandle const& final_display_ice_mask
)
{
    if (snapshot.v9)
    {
        if (!generation::isValidGenerationSnapshot(snapshot))
        {
            return std::nullopt;
        }
        generation::V9GenerationSnapshotFields const& source = *snapshot.v9;
        std::vector<float> elevation_km;
        elevation_km.reserve(source.source_elevation_m.size());
        for (float const value_m : source.source_elevation_m)
        {
            elevation_km.push_back(value_m / 1'000.0F);
        }
        std::vector<float> precipitation_m_year;
        precipitation_m_year.reserve(source.precipitation_mm_year.size());
        for (float const value_mm_year : source.precipitation_mm_year)
        {
            precipitation_m_year.push_back(value_mm_year / 1'000.0F);
        }
        std::vector<float> water_level_km;
        water_level_km.reserve(source.water_level_m.size());
        for (float const value_m : source.water_level_m)
        {
            water_level_km.push_back(value_m / 1'000.0F);
        }

        std::vector<int16_t> compact_plate_ids;
        std::vector<float> plate_vx;
        std::vector<float> plate_vy;
        if (source.plate_directions.size() <= 64U)
        {
            std::vector<generation::GenerationPlateDirection> directions = source.plate_directions;
            std::sort(directions.begin(), directions.end(), [](auto const& left, auto const& right) {
                return left.plate_id < right.plate_id;
            });
            compact_plate_ids.resize(source.plate_ids.size());
            plate_vx.resize(source.plate_ids.size());
            plate_vy.resize(source.plate_ids.size());
            for (size_t index = 0U; index < source.plate_ids.size(); ++index)
            {
                auto const direction = std::lower_bound(
                    directions.begin(),
                    directions.end(),
                    source.plate_ids[index],
                    [](generation::GenerationPlateDirection const& candidate, uint32_t const plate_id) {
                        return candidate.plate_id < plate_id;
                    }
                );
                if (direction == directions.end() || direction->plate_id != source.plate_ids[index])
                {
                    return std::nullopt;
                }
                compact_plate_ids[index] = static_cast<int16_t>(direction - directions.begin());
                plate_vx[index] = direction->x;
                plate_vy[index] = direction->y;
            }
        }
        AnimationSourceFields fields{
            .resolution = snapshot.resolution,
            .elevation = elevation_km,
            .temperature = source.temperature_celsius,
            .precipitation = precipitation_m_year,
            .water_level = water_level_km,
            .plate_vx = plate_vx,
            .plate_vy = plate_vy,
            .plate_id = compact_plate_ids,
            .ocean_mask = source.ocean_mask,
            .lake_mask = source.lake_mask,
            .river_mask = source.river_mask,
            .ice_kind = source.ice_surface_kind,
            .hydrology_available = snapshot.stage >= generation::GenerationStage::V9Water,
            .climate_available = !source.temperature_celsius.empty() || !source.precipitation_mm_year.empty(),
            .sea_level = 0.0F,
        };
        static_cast<void>(final_display_ice_mask);
        return prepareAnimationMap(fields);
    }

    GenerationMapRasterSource const source = rasterSource(snapshot);
    if (!hasValidResolution(source.resolution))
    {
        return std::nullopt;
    }
    uint32_t const pixel_count = source.resolution * source.resolution;
    bool uses_hydrology = false;
    bool uses_climate = false;
    if (snapshot.stage == generation::GenerationStage::Complete)
    {
        uses_hydrology = hasValidHydrology(source, pixel_count);
        uses_climate = hasValidClimate(source, pixel_count);
    }
    else if (!hasValidStageFields(snapshot.stage, source, pixel_count, uses_hydrology, uses_climate))
    {
        return std::nullopt;
    }
    AnimationSourceFields fields{
        .resolution = source.resolution,
        .elevation = source.elevation,
        .temperature = source.temperature,
        .precipitation = source.precipitation,
        .water_level = source.water_level,
        .ocean_mask = source.ocean_mask,
        .lake_mask = source.lake_mask,
        .river_mask = source.river_mask,
        .ice_kind = final_display_ice_mask
            ? std::span<uint8_t const>{ final_display_ice_mask->surface_kind }
            : std::span<uint8_t const>{},
        .ice_temperature = final_display_ice_mask
            ? std::span<float const>{ final_display_ice_mask->temperature }
            : std::span<float const>{},
        .hydrology_available = uses_hydrology,
        .climate_available = uses_climate,
    };
    return prepareAnimationMap(fields);
}

struct AnimationPhaseState final
{
    GenerationMapAnimationPhase phase = GenerationMapAnimationPhase::Tectonics;
    uint32_t phase_index = 0U;
    float progress = 0.0F;
};

[[nodiscard]]
AnimationPhaseState animationPhase(GenerationMapAnimationClock const& clock) noexcept
{
    int64_t const nonnegative_milliseconds = std::max<int64_t>(clock.visible_elapsed.count(), 0);
    static constexpr int64_t PHASE_DURATION_MILLISECONDS = 5'000;
    static constexpr int64_t ANIMATION_DURATION_MILLISECONDS = PHASE_DURATION_MILLISECONDS * 6;
    if (nonnegative_milliseconds >= ANIMATION_DURATION_MILLISECONDS)
    {
        return {
            .phase = GenerationMapAnimationPhase::Arrival,
            .phase_index = 5U,
            .progress = 1.0F,
        };
    }
    uint32_t const phase_index = static_cast<uint32_t>(nonnegative_milliseconds / PHASE_DURATION_MILLISECONDS);
    float const progress = static_cast<float>(nonnegative_milliseconds % PHASE_DURATION_MILLISECONDS)
        / static_cast<float>(PHASE_DURATION_MILLISECONDS);
    return {
        .phase = static_cast<GenerationMapAnimationPhase>(phase_index),
        .phase_index = phase_index,
        .progress = progress,
    };
}

[[nodiscard]]
float phaseProgressForRender(AnimationPhaseState const& phase, bool const reduced_motion) noexcept
{
    return reduced_motion ? 1.0F : smoothProgress(phase.progress);
}

[[nodiscard]]
float animationDrift(AnimationPhaseState const& phase, float const progress, bool const has_motion) noexcept
{
    if (!has_motion)
    {
        return 0.0F;
    }
    if (phase.phase_index == 0U)
    {
        return 3.0F - progress * 1.8F;
    }
    if (phase.phase_index == 1U)
    {
        return 1.2F * (1.0F - progress);
    }
    return 0.0F;
}

[[nodiscard]]
float animationHeightScale(AnimationPhaseState const& phase, float const progress) noexcept
{
    if (phase.phase_index == 0U)
    {
        return 0.85F + progress * 0.08F;
    }
    if (phase.phase_index == 1U)
    {
        return 0.93F + progress * 0.07F;
    }
    return 1.0F;
}

[[nodiscard]]
float animationSeaLevel(PreparedAnimationMap const& data, AnimationPhaseState const& phase, float const progress) noexcept
{
    if (phase.phase_index == 0U)
    {
        return data.sea_level - 0.3F + progress * 0.1F;
    }
    if (phase.phase_index == 1U)
    {
        return data.sea_level - 0.2F - progress * 0.45F;
    }
    if (phase.phase_index == 2U)
    {
        return data.sea_level - 0.65F * (1.0F - progress);
    }
    return data.sea_level;
}

[[nodiscard]]
float animationWaterProgress(AnimationPhaseState const& phase, float const progress) noexcept
{
    if (phase.phase_index < 2U)
    {
        return 0.0F;
    }
    if (phase.phase_index == 2U)
    {
        return progress;
    }
    return 1.0F;
}

[[nodiscard]]
float animationIceProgress(AnimationPhaseState const& phase, float const progress) noexcept
{
    if (phase.phase_index < 3U)
    {
        return 0.0F;
    }
    if (phase.phase_index == 3U)
    {
        return progress;
    }
    return 1.0F;
}

[[nodiscard]]
float animationClimateProgress(AnimationPhaseState const& phase, float const progress) noexcept
{
    return phase.phase_index == 4U ? progress : 0.0F;
}

[[nodiscard]]
float animationUpliftProgress(AnimationPhaseState const& phase, float const progress, bool const has_motion) noexcept
{
    if (!has_motion)
    {
        return 0.0F;
    }
    if (phase.phase_index == 0U)
    {
        return progress * 0.8F;
    }
    if (phase.phase_index == 1U)
    {
        return (1.0F - progress) * 0.8F;
    }
    return 0.0F;
}

[[nodiscard]]
bool animationWaterAt(
    AnimationCell const& cell,
    float const elevation,
    float const sea_level,
    float const target_sea_level,
    float const drainage,
    bool const hydrology_available
) noexcept
{
    bool wet = hydrology_available && drainage > 0.0F && elevation <= sea_level;
    if ((cell.water_mask & 1U) != 0U && drainage > 0.0F)
    {
        wet = wet || elevation <= sea_level + (target_sea_level - sea_level) * drainage;
    }
    if ((cell.water_mask & 2U) != 0U && drainage > 0.0F)
    {
        wet = wet || elevation <= sea_level + (cell.water_level - sea_level) * drainage;
    }
    if ((cell.water_mask & 4U) != 0U && drainage > 0.0F)
    {
        wet = true;
    }
    return wet;
}

void blendPixel(GenerationMapRgba8& pixel, GenerationMapRgba8 const& tint, float const alpha) noexcept
{
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        pixel[channel] = static_cast<uint8_t>(std::clamp(
            static_cast<float>(pixel[channel])
                + (static_cast<float>(tint[channel]) - static_cast<float>(pixel[channel])) * alpha,
            0.0F,
            255.0F
        ));
    }
}

void animationPlot(
    GenerationMapImage& image,
    PreparedAnimationMap const& data,
    float const x,
    float const y,
    GenerationMapRgba8 const& color,
    float const alpha,
    uint8_t const kind
) noexcept
{
    if (x < 0.0F || y < 0.0F || x >= static_cast<float>(data.resolution)
        || y >= static_cast<float>(data.resolution))
    {
        return;
    }
    uint32_t const source_x = static_cast<uint32_t>(x);
    uint32_t const source_y = static_cast<uint32_t>(y);
    AnimationCell const& source = data.cells[animationCellIndex(source_x, source_y)];
    if (source.ice_kind != GENERATION_MAP_ICE_NONE
        || (kind == GENERATION_MAP_ICE_WATER && (source.water_mask & 3U) == 0U)
        || (kind == 3U && source.wetness < 0.22F))
    {
        return;
    }
    uint32_t const render_x = std::min(
        GENERATION_MAP_ANIMATION_RENDER_RESOLUTION - 1U,
        static_cast<uint32_t>(std::lround(x * static_cast<float>(GENERATION_MAP_ANIMATION_SCALE)))
    );
    uint32_t const render_y = std::min(
        GENERATION_MAP_ANIMATION_RENDER_RESOLUTION - 1U,
        static_cast<uint32_t>(std::lround(y * static_cast<float>(GENERATION_MAP_ANIMATION_SCALE)))
    );
    GenerationMapRgba8& pixel = image.rgba[
        render_y * GENERATION_MAP_ANIMATION_RENDER_RESOLUTION + render_x
    ];
    blendPixel(pixel, color, alpha);
}

void animationLine(
    GenerationMapImage& image,
    PreparedAnimationMap const& data,
    AnimationPoint const& from,
    AnimationPoint const& to,
    GenerationMapRgba8 const& color,
    float const alpha,
    uint8_t const kind
) noexcept
{
    float const distance = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    uint32_t const steps = std::max(
        1U,
        static_cast<uint32_t>(std::ceil(distance * static_cast<float>(GENERATION_MAP_ANIMATION_SCALE)))
    );
    for (uint32_t step = 0U; step <= steps; ++step)
    {
        float const amount = static_cast<float>(step) / static_cast<float>(steps);
        animationPlot(
            image,
            data,
            from.x + (to.x - from.x) * amount,
            from.y + (to.y - from.y) * amount,
            color,
            alpha,
            kind
        );
    }
}

void animateWeather(GenerationMapImage& image, PreparedAnimationMap const& data, float const progress)
{
    float const opacity = std::clamp(progress * 5.0F, 0.0F, 1.0F);
    if (opacity <= 0.0F)
    {
        return;
    }
    auto drawPaths = [&](std::vector<AnimationPath> const& paths, GenerationMapRgba8 const& color, uint8_t kind) {
        for (AnimationPath const& path : paths)
        {
            float const head = std::fmod(progress * 26.0F + path.phase, static_cast<float>(path.points.size() - 1U));
            for (int32_t tail = 5; tail >= 0; --tail)
            {
                float const position = head - static_cast<float>(tail);
                if (position < 0.0F)
                {
                    continue;
                }
                uint32_t const index = static_cast<uint32_t>(position);
                uint32_t const next = std::min(static_cast<uint32_t>(path.points.size() - 1U), index + 1U);
                float const fraction = tail == 0 ? position - static_cast<float>(index) : 1.0F;
                AnimationPoint const& from = path.points[index];
                AnimationPoint const& next_point = path.points[next];
                AnimationPoint const to{
                    .x = from.x + (next_point.x - from.x) * fraction,
                    .y = from.y + (next_point.y - from.y) * fraction,
                };
                animationLine(
                    image,
                    data,
                    from,
                    to,
                    color,
                    (0.2F + static_cast<float>(5 - tail) * 0.1F) * opacity,
                    kind
                );
            }
        }
    };
    drawPaths(data.wind_paths, { 99U, 219U, 212U, 255U }, 0U);
    drawPaths(data.current_paths, { 220U, 238U, 247U, 255U }, GENERATION_MAP_ICE_WATER);
    for (AnimationRainDrop const& drop : data.rain_drops)
    {
        AnimationCell const& cell = data.cells[drop.cell_index];
        float const fraction = std::fmod(progress * 4.0F + drop.phase, 1.0F);
        float const magnitude = std::hypot(cell.wind_u, cell.wind_v);
        float const wind_x = magnitude > 1.0e-8F ? cell.wind_u / magnitude : 0.0F;
        float const wind_y = magnitude > 1.0e-8F ? cell.wind_v / magnitude : 0.0F;
        float const x = static_cast<float>(drop.cell_index % data.resolution) + wind_x * fraction * 1.5F;
        float const y = static_cast<float>(drop.cell_index / data.resolution)
            + wind_y * fraction * 1.5F + fraction * 2.0F;
        animationLine(
            image,
            data,
            { x, y },
            { x + wind_x * 0.35F, y + wind_y * 0.35F + 0.65F },
            { 176U, 211U, 228U, 255U },
            0.68F * opacity,
            3U
        );
    }
}

[[nodiscard]]
float earlyEdgeSoftening(AnimationPhaseState const& phase, float const progress) noexcept
{
    if (phase.phase_index == 0U)
    {
        return 0.28F - 0.08F * progress;
    }
    if (phase.phase_index == 1U)
    {
        return 0.20F * (1.0F - progress);
    }
    return 0.0F;
}

[[nodiscard]]
uint32_t maximumColorDifference(GenerationMapRgba8 const& left, GenerationMapRgba8 const& right) noexcept
{
    uint32_t difference = 0U;
    for (uint32_t channel = 0U; channel < 3U; ++channel)
    {
        uint32_t const left_value = left[channel];
        uint32_t const right_value = right[channel];
        difference = std::max(difference, left_value > right_value ? left_value - right_value : right_value - left_value);
    }
    return difference;
}

void softenEarlyImageEdges(
    GenerationMapImage& image,
    std::span<int32_t const> source_map,
    PreparedAnimationMap const& data,
    float const strength
)
{
    if (strength <= 0.0F)
    {
        return;
    }
    static constexpr uint32_t EDGE_COLOR_DIFFERENCE = 18U;
    uint32_t const width = image.width;
    uint32_t const height = image.height;
    std::vector<GenerationMapRgba8> const source_pixels = image.rgba;
    for (uint32_t y = 0U; y < height; ++y)
    {
        for (uint32_t x = 0U; x < width; ++x)
        {
            uint32_t const index = y * width + x;
            std::array<uint32_t, 4U> const neighbors{
                y * width + (x + width - 1U) % width,
                y * width + (x + 1U) % width,
                (y + height - 1U) % height * width + x,
                (y + 1U) % height * width + x,
            };
            bool hard_edge = std::any_of(neighbors.begin(), neighbors.end(), [&](uint32_t const neighbor) {
                return maximumColorDifference(source_pixels[index], source_pixels[neighbor])
                    >= EDGE_COLOR_DIFFERENCE;
            });
            int32_t const source_index = source_map[index];
            if (!hard_edge && source_index >= 0)
            {
                int16_t const plate_index = data.cells[static_cast<size_t>(source_index)].plate_index;
                if (plate_index >= 0)
                {
                    hard_edge = std::any_of(neighbors.begin(), neighbors.end(), [&](uint32_t const neighbor) {
                        int32_t const neighbor_source_index = source_map[neighbor];
                        return neighbor_source_index >= 0
                            && data.cells[static_cast<size_t>(neighbor_source_index)].plate_index >= 0
                            && data.cells[static_cast<size_t>(neighbor_source_index)].plate_index != plate_index;
                    });
                }
            }
            if (!hard_edge)
            {
                continue;
            }

            GenerationMapRgba8 softened_pixel = source_pixels[index];
            for (uint32_t channel = 0U; channel < 3U; ++channel)
            {
                uint32_t total = 0U;
                for (uint32_t offset_y = 0U; offset_y < 3U; ++offset_y)
                {
                    uint32_t const source_y = (y + height + offset_y - 1U) % height;
                    for (uint32_t offset_x = 0U; offset_x < 3U; ++offset_x)
                    {
                        uint32_t const source_x = (x + width + offset_x - 1U) % width;
                        total += source_pixels[source_y * width + source_x][channel];
                    }
                }
                softened_pixel[channel] = static_cast<uint8_t>((total + 4U) / 9U);
            }
            blendPixel(image.rgba[index], softened_pixel, strength);
        }
    }
}

[[nodiscard]]
GenerationMapImage renderAnimationFrame(
    PreparedAnimationMap const& data,
    AnimationPhaseState const& phase,
    bool const reduced_motion
)
{
    float const progress = phaseProgressForRender(phase, reduced_motion);
    float const drift = animationDrift(phase, progress, data.has_motion);
    float const height_scale = animationHeightScale(phase, progress);
    float const sea_level = animationSeaLevel(data, phase, progress);
    float const water_progress = animationWaterProgress(phase, progress);
    float const ice_progress = animationIceProgress(phase, progress);
    float const climate_progress = animationClimateProgress(phase, progress);
    float const uplift = animationUpliftProgress(phase, progress, data.has_motion);
    static constexpr uint32_t RASTER_OVERSAMPLE = 2U;
    bool const samples_fractional_drift = data.has_motion && drift > 0.0F;
    uint32_t const raster_oversample = samples_fractional_drift ? RASTER_OVERSAMPLE : 1U;
    uint32_t const render_resolution = GENERATION_MAP_ANIMATION_RENDER_RESOLUTION;
    uint32_t const render_count = render_resolution * render_resolution;
    uint32_t const raster_scale = GENERATION_MAP_ANIMATION_SCALE * raster_oversample;
    uint32_t const raster_resolution = render_resolution * raster_oversample;
    uint32_t const raster_count = raster_resolution * raster_resolution;
    std::vector<int32_t> raster_source_map(raster_count, -1);
    std::vector<float> height_map(raster_count, -std::numeric_limits<float>::infinity());
    std::vector<uint8_t> raster_fallback_map(samples_fractional_drift ? raster_count : 0U, 0U);
    std::vector<GenerationMapRgba8> motion_colors;
    std::vector<GenerationMapRgba8> motion_fallback_colors;
    std::vector<float> source_slopes;
    if (samples_fractional_drift)
    {
        // Drift phases precede water, climate, and ice composition.
        uint32_t const cell_count = data.resolution * data.resolution;
        motion_colors.resize(cell_count);
        motion_fallback_colors.resize(cell_count);
        source_slopes.resize(cell_count);
        auto const source_height = [&data, height_scale, uplift](uint32_t const sample_x, uint32_t const sample_y) {
            AnimationCell const& sample = data.cells[animationCellIndex(sample_x, sample_y)];
            return sample.elevation * height_scale + sample.strain * uplift;
        };
        for (uint32_t y = 0U; y < data.resolution; ++y)
        {
            for (uint32_t x = 0U; x < data.resolution; ++x)
            {
                uint32_t const index = animationCellIndex(x, y);
                AnimationCell const& cell = data.cells[index];
                float const height = cell.elevation * height_scale + cell.strain * uplift;
                float const fallback_height = std::min(cell.elevation * height_scale, sea_level - 0.2F);
                motion_colors[index] = elevationColor(height);
                motion_fallback_colors[index] = elevationColor(fallback_height);
                source_slopes[index] = source_height((x + data.resolution - 1U) % data.resolution, y)
                    - source_height((x + 1U) % data.resolution, y)
                    + 0.65F * (
                        source_height(x, (y + data.resolution - 1U) % data.resolution)
                        - source_height(x, (y + 1U) % data.resolution)
                    );
            }
        }
    }
    for (uint32_t y = 0U; y < data.resolution; ++y)
    {
        for (uint32_t x = 0U; x < data.resolution; ++x)
        {
            uint32_t const source_index = animationCellIndex(x, y);
            AnimationCell const& cell = data.cells[source_index];
            float const height = cell.elevation * height_scale + cell.strain * uplift;
            int32_t const target_x = static_cast<int32_t>(std::lround(
                (static_cast<float>(x) - cell.plate_u * drift) * static_cast<float>(raster_scale)
            ));
            int32_t const target_y = static_cast<int32_t>(std::lround(
                (static_cast<float>(y) - cell.plate_v * drift) * static_cast<float>(raster_scale)
            ));
            for (uint32_t offset_y = 0U; offset_y < raster_scale; ++offset_y)
            {
                for (uint32_t offset_x = 0U; offset_x < raster_scale; ++offset_x)
                {
                    int32_t pixel_x = target_x + static_cast<int32_t>(offset_x);
                    int32_t pixel_y = target_y + static_cast<int32_t>(offset_y);
                    pixel_x = (pixel_x % static_cast<int32_t>(raster_resolution) + static_cast<int32_t>(raster_resolution))
                        % static_cast<int32_t>(raster_resolution);
                    pixel_y = (pixel_y % static_cast<int32_t>(raster_resolution) + static_cast<int32_t>(raster_resolution))
                        % static_cast<int32_t>(raster_resolution);
                    uint32_t const target_index = static_cast<uint32_t>(pixel_y) * raster_resolution
                        + static_cast<uint32_t>(pixel_x);
                    if (height <= height_map[target_index])
                    {
                        continue;
                    }
                    raster_source_map[target_index] = static_cast<int32_t>(source_index);
                    height_map[target_index] = height;
                }
            }
        }
    }
    for (uint32_t index = 0U; index < raster_count; ++index)
    {
        if (raster_source_map[index] >= 0)
        {
            continue;
        }
        uint32_t const native_x = (index % raster_resolution) / raster_scale;
        uint32_t const native_y = (index / raster_resolution) / raster_scale;
        uint32_t const native_index = animationCellIndex(native_x, native_y);
        int32_t source_index = static_cast<int32_t>(native_index);
        if (!data.nearest_water.empty() && data.nearest_water[native_index] >= 0)
        {
            source_index = data.nearest_water[native_index];
        }
        AnimationCell const& cell = data.cells[static_cast<size_t>(source_index)];
        raster_source_map[index] = source_index;
        height_map[index] = std::min(cell.elevation * height_scale, sea_level - 0.2F);
        if (samples_fractional_drift)
        {
            raster_fallback_map[index] = 1U;
        }
    }

    GenerationMapImage image;
    image.width = render_resolution;
    image.height = render_resolution;
    image.rgba.resize(render_count);
    std::vector<int32_t> source_map(render_count, -1);
    uint32_t const downsample_pixel_count = raster_oversample * raster_oversample;
    uint32_t const center_sample = raster_oversample / 2U;
    for (uint32_t output_index = 0U; output_index < render_count; ++output_index)
    {
        uint32_t const x = output_index % render_resolution;
        uint32_t const y = output_index / render_resolution;
        std::array<uint32_t, 3U> channel_totals{};
        for (uint32_t sample_index = 0U; sample_index < downsample_pixel_count; ++sample_index)
        {
            uint32_t const raster_x = x * raster_oversample + sample_index % raster_oversample;
            uint32_t const raster_y = y * raster_oversample + sample_index / raster_oversample;
            uint32_t const raster_index = raster_y * raster_resolution + raster_x;
            int32_t const mapped_index = raster_source_map[raster_index];
            uint32_t const source_index = static_cast<uint32_t>(mapped_index);
            uint32_t const native_x = raster_x / raster_scale;
            uint32_t const native_y = raster_y / raster_scale;
            uint32_t const native_index = animationCellIndex(native_x, native_y);
            AnimationCell const& source_cell = data.cells[source_index];
            AnimationCell const& native_cell = data.cells[native_index];
            float const elevation = height_map[raster_index];
            bool wet = false;
            GenerationMapRgba8 color{};
            float original_slope = 0.0F;
            if (samples_fractional_drift)
            {
                bool const is_fallback = raster_fallback_map[raster_index] != 0U;
                wet = is_fallback || (data.has_hydrology && source_cell.water_mask != 0U);
                color = is_fallback ? motion_fallback_colors[source_index] : motion_colors[source_index];
                original_slope = source_slopes[source_index];
            }
            else
            {
                wet = animationWaterAt(
                    source_cell,
                    elevation,
                    sea_level,
                    data.sea_level,
                    water_progress,
                    data.has_hydrology
                );
                color = elevationColor(elevation);
                if (wet)
                {
                    if (source_cell.water_mask != 0U)
                    {
                        color = waterColor(
                            color,
                            elevation,
                            source_cell.water_level,
                            (source_cell.water_mask & 1U) != 0U ? 1U : 0U,
                            (source_cell.water_mask & 2U) != 0U ? 1U : 0U,
                            (source_cell.water_mask & 4U) != 0U ? 1U : 0U,
                            1.0F
                        );
                    }
                    else
                    {
                        blendPixel(color, { 44U, 97U, 121U, 255U }, std::max(water_progress, 0.72F));
                    }
                }
                if (data.has_climate && climate_progress > 0.0F)
                {
                    color = blendColor(
                        color,
                        climateColor(
                            native_cell.temperature,
                            native_cell.precipitation,
                            data.has_temperature,
                            data.has_precipitation
                        ),
                        0.32F * climate_progress
                    );
                }
                if (data.has_ice && ice_progress > 0.0F && native_cell.ice_kind != GENERATION_MAP_ICE_NONE)
                {
                    float const ice_limit = data.minimum_ice_temperature
                        + (data.maximum_ice_temperature - data.minimum_ice_temperature) * ice_progress;
                    bool const target_reached = native_cell.ice_temperature <= ice_limit;
                    if (target_reached)
                    {
                        color = frozenSurfaceColor(color, native_cell.ice_kind, 1.0F);
                    }
                }
                original_slope = data.cells[animationCellIndex(
                    (native_x + data.resolution - 1U) % data.resolution,
                    native_y
                )].elevation - data.cells[animationCellIndex(
                    (native_x + 1U) % data.resolution,
                    native_y
                )].elevation + 0.65F * (
                    data.cells[animationCellIndex(
                        native_x,
                        (native_y + data.resolution - 1U) % data.resolution
                    )].elevation - data.cells[animationCellIndex(native_x, (native_y + 1U) % data.resolution)].elevation
                );
            }
            uint32_t const west = raster_y * raster_resolution
                + (raster_x + raster_resolution - raster_scale) % raster_resolution;
            uint32_t const east = raster_y * raster_resolution
                + (raster_x + raster_scale) % raster_resolution;
            uint32_t const north = ((raster_y + raster_resolution - raster_scale) % raster_resolution)
                * raster_resolution + raster_x;
            uint32_t const south = ((raster_y + raster_scale) % raster_resolution) * raster_resolution + raster_x;
            float const slope = height_map[west] - height_map[east]
                + (height_map[north] - height_map[south]) * 0.65F;
            float const shade = wet
                ? 1.0F
                : std::clamp(1.0F + (slope - original_slope) * 4.0F / data.elevation_range, 0.65F, 1.25F);
            for (uint32_t channel = 0U; channel < 3U; ++channel)
            {
                color[channel] = static_cast<uint8_t>(std::clamp(
                    static_cast<float>(color[channel]) * shade,
                    0.0F,
                    255.0F
                ));
                channel_totals[channel] += color[channel];
            }
            if (sample_index == center_sample * raster_oversample + center_sample)
            {
                source_map[output_index] = mapped_index;
            }
        }
        GenerationMapRgba8& output_pixel = image.rgba[output_index];
        for (uint32_t channel = 0U; channel < 3U; ++channel)
        {
            output_pixel[channel] = static_cast<uint8_t>(
                (channel_totals[channel] + downsample_pixel_count / 2U) / downsample_pixel_count
            );
        }
        output_pixel[3U] = 255U;
    }
    if (phase.phase_index == 4U && (data.has_wind || data.has_currents || !data.rain_drops.empty()))
    {
        animateWeather(image, data, phaseProgressForRender(phase, reduced_motion));
    }
    softenEarlyImageEdges(
        image,
        source_map,
        data,
        earlyEdgeSoftening(phase, progress)
    );
    return image;
}

} // namespace

std::optional<GenerationMapDisplayIceMask> makeGenerationMapDisplayIceMask(
    std::span<uint8_t const> const climate_zone,
    std::span<uint8_t const> const ocean_mask,
    std::span<uint8_t const> const lake_mask,
    std::span<float const> const temperature
)
{
    if (climate_zone.empty()
        || climate_zone.size() > static_cast<decltype(climate_zone.size())>(std::numeric_limits<uint32_t>::max())
        || ocean_mask.size() != climate_zone.size()
        || lake_mask.size() != climate_zone.size()
        || temperature.size() != climate_zone.size()
        || !isByteMask(ocean_mask)
        || !isByteMask(lake_mask)
        || !isFiniteSpan(temperature)
        || !std::all_of(climate_zone.begin(), climate_zone.end(), [](uint8_t const value) {
            return value <= SNOW_CLIMATE_ZONE;
        }))
    {
        return std::nullopt;
    }

    GenerationMapDisplayIceMask mask;
    mask.surface_kind.resize(climate_zone.size(), GENERATION_MAP_ICE_NONE);
    mask.temperature.assign(temperature.begin(), temperature.end());
    for (uint32_t index = 0U; index < static_cast<uint32_t>(climate_zone.size()); ++index)
    {
        bool const water = ocean_mask[index] != 0U || lake_mask[index] != 0U;
        if (!water && climate_zone[index] == SNOW_CLIMATE_ZONE)
        {
            mask.surface_kind[index] = GENERATION_MAP_ICE_SNOW;
        }
        else if (water && temperature[index] <= WATER_ICE_TEMPERATURE_C)
        {
            mask.surface_kind[index] = GENERATION_MAP_ICE_WATER;
        }
    }
    return mask;
}

std::optional<GenerationMapImage> rasterizeGenerationMap(
    generation::GenerationSnapshot const& snapshot,
    std::span<uint8_t const> const display_ice_surface_kind
)
{
    return rasterizeGenerationMapInternal(
        snapshot,
        display_ice_surface_kind,
        {},
        RasterOptions{
            .output_resolution = snapshot.resolution,
        }
    );
}

std::optional<GenerationMapImage> rasterizeGenerationMap(
    generation::GlobalAtlas const& atlas,
    std::span<uint8_t const> const display_ice_surface_kind
)
{
    GenerationMapRasterSource const source = rasterSource(atlas);
    if (!hasValidResolution(source.resolution))
    {
        return std::nullopt;
    }
    uint32_t const pixel_count = source.resolution * source.resolution;
    if (!hasValidElevation(source, pixel_count)
        || !hasValidHydrology(source, pixel_count)
        || !hasValidClimate(source, pixel_count))
    {
        return std::nullopt;
    }

    return rasterizeGenerationMapFromSource(
        source,
        true,
        true,
        display_ice_surface_kind,
        {},
        RasterOptions{
            .output_resolution = source.resolution,
        }
    );
}

struct GenerationMapAnimationCache final
{
    std::shared_ptr<generation::GenerationSnapshot const> snapshot;
    std::shared_ptr<generation::GlobalAtlas const> atlas;
    GenerationMapDisplayIceMaskHandle display_ice_mask;
    std::optional<PreparedAnimationMap> prepared;
    bool uses_atlas = false;
};

GenerationMapAnimator::GenerationMapAnimator() = default;
GenerationMapAnimator::~GenerationMapAnimator() = default;
GenerationMapAnimator::GenerationMapAnimator(GenerationMapAnimator&&) noexcept = default;
GenerationMapAnimator& GenerationMapAnimator::operator=(GenerationMapAnimator&&) noexcept = default;

void GenerationMapAnimator::reset() noexcept
{
    cache_.reset();
}

GenerationMapAnimationFrame GenerationMapAnimator::update(
    std::shared_ptr<generation::GenerationSnapshot const> snapshot,
    std::shared_ptr<generation::GlobalAtlas const> matching_atlas,
    GenerationMapImageHandle final_image,
    GenerationMapDisplayIceMaskHandle final_display_ice_mask,
    GenerationMapAnimationClock const& clock
)
{
    AnimationPhaseState const phase = animationPhase(clock);
    GenerationMapAnimationFrame frame{
        .phase = phase.phase,
        .phase_progress = phase.progress,
    };
    if (phase.phase == GenerationMapAnimationPhase::Arrival && final_image != nullptr)
    {
        frame.image = std::move(final_image);
        return frame;
    }
    if (clock.reduced_motion)
    {
        if (final_image != nullptr)
        {
            frame.image = std::move(final_image);
            return frame;
        }
        std::optional<GenerationMapImage> image;
        if (matching_atlas != nullptr)
        {
            image = rasterizeGenerationMap(
                *matching_atlas,
                final_display_ice_mask
                    ? std::span<uint8_t const>{ final_display_ice_mask->surface_kind }
                    : std::span<uint8_t const>{}
            );
        }
        if (!image && snapshot != nullptr)
        {
            image = rasterizeGenerationMap(
                *snapshot,
                final_display_ice_mask
                    ? std::span<uint8_t const>{ final_display_ice_mask->surface_kind }
                    : std::span<uint8_t const>{}
            );
        }
        if (image)
        {
            frame.image = std::make_shared<GenerationMapImage const>(std::move(*image));
        }
        return frame;
    }

    if (matching_atlas != nullptr)
    {
        bool const cache_matches = cache_ != nullptr && cache_->uses_atlas
            && cache_->atlas == matching_atlas && cache_->display_ice_mask == final_display_ice_mask;
        if (!cache_matches)
        {
            std::optional<PreparedAnimationMap> prepared = prepareAnimationMap(*matching_atlas, final_display_ice_mask);
            if (prepared)
            {
                cache_ = std::make_unique<GenerationMapAnimationCache>(GenerationMapAnimationCache{
                    .atlas = matching_atlas,
                    .display_ice_mask = final_display_ice_mask,
                    .prepared = std::move(prepared),
                    .uses_atlas = true,
                });
            }
            else
            {
                cache_.reset();
            }
        }
    }

    if ((cache_ == nullptr || !cache_->prepared || !cache_->uses_atlas)
        && snapshot != nullptr)
    {
        bool const cache_matches = cache_ != nullptr && !cache_->uses_atlas
            && cache_->snapshot == snapshot && cache_->display_ice_mask == final_display_ice_mask;
        if (!cache_matches)
        {
            std::optional<PreparedAnimationMap> prepared = prepareAnimationMap(*snapshot, final_display_ice_mask);
            cache_ = std::make_unique<GenerationMapAnimationCache>(GenerationMapAnimationCache{
                .snapshot = snapshot,
                .atlas = matching_atlas,
                .display_ice_mask = final_display_ice_mask,
                .prepared = std::move(prepared),
                .uses_atlas = false,
            });
        }
    }

    if (cache_ == nullptr || !cache_->prepared)
    {
        return frame;
    }
    GenerationMapImage rendered = renderAnimationFrame(*cache_->prepared, phase, clock.reduced_motion);
    frame.image = std::make_shared<GenerationMapImage const>(std::move(rendered));
    return frame;
}

GenerationMapImageHandle animateGenerationMap(
    generation::GenerationSnapshot const& snapshot,
    GenerationMapImageHandle final_image,
    GenerationMapDisplayIceMaskHandle final_display_ice_mask,
    GenerationMapAnimationClock const& clock
)
{
    AnimationPhaseState const phase = animationPhase(clock);
    if (clock.ready || (phase.phase == GenerationMapAnimationPhase::Arrival && final_image != nullptr)) {
        return final_image;
    }
    if (clock.reduced_motion) {
        if (final_image != nullptr) {
            return final_image;
        }
        std::optional<GenerationMapImage> image = rasterizeGenerationMap(
            snapshot,
            final_display_ice_mask
                ? std::span<uint8_t const>{ final_display_ice_mask->surface_kind }
                : std::span<uint8_t const>{}
        );
        if (!image) {
            return nullptr;
        }
        return std::make_shared<GenerationMapImage const>(std::move(*image));
    }
    std::optional<PreparedAnimationMap> const prepared = prepareAnimationMap(snapshot, final_display_ice_mask);
    if (!prepared) {
        return nullptr;
    }
    return std::make_shared<GenerationMapImage const>(
        renderAnimationFrame(*prepared, phase, clock.reduced_motion)
    );
}

} // namespace client
