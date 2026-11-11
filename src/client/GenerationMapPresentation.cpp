#include <client/GenerationMapPresentation.hpp>

#include <client/GenerationMapHistory.hpp>
#include <client/S2MapPresentation.hpp>

#include <generation/V10Provider.hpp>
#include <generation/V9Provider.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <queue>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace client
{
namespace
{

static constexpr uint32_t MINIMUM_RESOLUTION = 256U;
static constexpr uint32_t MAXIMUM_RESOLUTION = 512U;
static constexpr std::array<uint32_t, 5U> ANIMATION_PHASE_WEIGHTS{30U, 20U, 15U, 15U, 20U};
static constexpr std::array<uint32_t, 5U> NATIVE_ANIMATION_PHASE_WEIGHTS{20U, 15U, 25U, 30U, 10U};
static constexpr uint8_t SNOW_CLIMATE_ZONE = 5U;
static constexpr float WATER_ICE_TEMPERATURE_C = -10.0F;
static constexpr uint8_t GENERATION_MAP_ICE_GLACIER = 2U;
static constexpr uint8_t GENERATION_MAP_ICE_SEA = 3U;

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
    std::vector<float> temperature_celsius;
    std::vector<float> precipitation_m_year;
    std::vector<float> water_level_km;
    std::vector<float> ice_temperature_celsius;
    std::vector<uint8_t> v21_ice_surface_kind;
};

template<typename TValue>
[[nodiscard]] std::span<TValue const> v21GridField(
    std::span<generation::V9RegisteredFieldView const> fields,
    std::string_view const name,
    generation::V9FieldType const type,
    std::span<uint32_t const> expected_shape
) noexcept
{
    auto const found = std::find_if(fields.begin(), fields.end(), [name](auto const& field) {
        return field.name == name;
    });
    if (found == fields.end() || found->type != type || !std::ranges::equal(found->shape, expected_shape))
    {
        return {};
    }
    size_t expected_count = 1U;
    for (uint32_t const extent : expected_shape)
    {
        if (extent == 0U || expected_count > std::numeric_limits<size_t>::max() / extent)
        {
            return {};
        }
        expected_count *= extent;
    }
    auto const* const values = std::get_if<std::span<TValue const>>(&found->values);
    return values != nullptr && values->size() == expected_count ? *values : std::span<TValue const>{};
}

template<typename TValue>
[[nodiscard]]
std::vector<float> v21MeanPhaseField(std::span<TValue const> values, size_t const field_size)
{
    std::vector<float> result(field_size);
    for (size_t index = 0U; index < field_size; ++index)
    {
        double total = 0.0;
        for (size_t phase = 0U; phase < 8U; ++phase)
        {
            total += values[phase * field_size + index];
        }
        result[index] = static_cast<float>(total / 8.0);
    }
    return result;
}

[[nodiscard]]
std::vector<float> expandV21ClimateField(std::span<float const> climate_field)
{
    constexpr uint32_t SOURCE_RESOLUTION = 256U;
    constexpr uint32_t CLIMATE_RESOLUTION = 32U;
    constexpr size_t CLIMATE_COUNT = static_cast<size_t>(CLIMATE_RESOLUTION) * CLIMATE_RESOLUTION;
    if (climate_field.size() != CLIMATE_COUNT)
    {
        return {};
    }
    std::vector<float> result(static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION);
    for (uint32_t y = 0U; y < SOURCE_RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < SOURCE_RESOLUTION; ++x)
        {
            result[static_cast<size_t>(y) * SOURCE_RESOLUTION + x]
                = climate_field[static_cast<size_t>(y / 8U) * CLIMATE_RESOLUTION + x / 8U];
        }
    }
    return result;
}

[[nodiscard]]
std::vector<uint8_t> nativeStockIceSurfaceKind(
    std::span<float const> snow,
    std::span<float const> glacier,
    std::span<float const> sea_ice,
    std::span<uint8_t const> ocean,
    std::span<uint8_t const> lake,
    std::span<uint8_t const> river,
    size_t const field_size
)
{
    std::vector<uint8_t> result(field_size, GENERATION_MAP_ICE_NONE);
    constexpr float DISPLAY_ICE_CUTOFF_M = 0.1F;
    for (size_t index = 0U; index < field_size; ++index)
    {
        bool const wet = ocean[index] != 0U || lake[index] != 0U || river[index] != 0U;
        if (ocean[index] != 0U && sea_ice[index] >= DISPLAY_ICE_CUTOFF_M)
        {
            result[index] = GENERATION_MAP_ICE_WATER;
        }
        else if (!wet && snow[index] >= DISPLAY_ICE_CUTOFF_M)
        {
            result[index] = GENERATION_MAP_ICE_SNOW;
        }
        else if (!wet && glacier[index] >= DISPLAY_ICE_CUTOFF_M)
        {
            // Water context distinguishes glacier from sea ice within the V21 display raster.
            result[index] = GENERATION_MAP_ICE_WATER;
        }
    }
    return result;
}

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

static constexpr GenerationMapRgba8 COOLING_WATER_CRUST_COLOR{68U, 61U, 52U, 255U};

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
    case generation::GenerationStage::V21Platec:
    case generation::GenerationStage::V21Terrain:
    case generation::GenerationStage::V10Platec:
    case generation::GenerationStage::V10Terrain:
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
    case generation::GenerationStage::V21Water:
    case generation::GenerationStage::V10Water:
    case generation::GenerationStage::Complete:
        uses_hydrology = hasValidHydrology(source, pixel_count);
        uses_climate = hasValidClimate(source, pixel_count);
        return uses_hydrology && uses_climate;
    case generation::GenerationStage::V21Climate:
    case generation::GenerationStage::V10Climate:
        uses_climate = isFiniteField(source.temperature, pixel_count)
            && (source.precipitation.empty() || isFiniteField(source.precipitation, pixel_count));
        return uses_climate;
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
    if (snapshot.v21)
    {
        generation::V21GenerationSnapshotFields const& fields = *snapshot.v21;
        size_t const field_count = static_cast<size_t>(snapshot.resolution) * snapshot.resolution;
        backing.elevation_km.reserve(fields.source_elevation_m.size());
        for (float const elevation_m : fields.source_elevation_m)
        {
            backing.elevation_km.push_back(elevation_m / 1'000.0F);
        }
        if (fields.phase_temperature_celsius.size() == field_count * 8U)
        {
            backing.temperature_celsius = v21MeanPhaseField<float>(fields.phase_temperature_celsius, field_count);
            backing.ice_temperature_celsius.assign(fields.phase_temperature_celsius.begin(),
                fields.phase_temperature_celsius.begin()
                    + static_cast<std::vector<float>::difference_type>(field_count));
        }
        else
        {
            backing.temperature_celsius.assign(fields.temperature_celsius.begin(), fields.temperature_celsius.end());
        }
        if (fields.phase_precipitation_mm_year.size() == field_count * 8U)
        {
            backing.precipitation_m_year = v21MeanPhaseField<float>(fields.phase_precipitation_mm_year, field_count);
            for (float& value : backing.precipitation_m_year)
            {
                value /= 1'000.0F;
            }
        }
        else
        {
            backing.precipitation_m_year.reserve(fields.precipitation_mm_year.size());
            for (float const value_mm_year : fields.precipitation_mm_year)
            {
                backing.precipitation_m_year.push_back(value_mm_year / 1'000.0F);
            }
        }
        backing.water_level_km.reserve(fields.water_level_blocks.size());
        for (float const level_blocks : fields.water_level_blocks)
        {
            backing.water_level_km.push_back(
                level_blocks / 0.34F / 1'000.0F
            );
        }
        if (fields.land_snow_water_equivalent_m.size() == field_count * 8U
            && fields.glacier_thickness_m.size() == field_count * 8U
            && fields.sea_ice_thickness_m.size() == field_count * 8U
            && fields.ocean_mask.size() == field_count && fields.lake_mask.size() == field_count
            && fields.river_mask.size() == field_count)
        {
            backing.v21_ice_surface_kind = nativeStockIceSurfaceKind(
                fields.land_snow_water_equivalent_m,
                fields.glacier_thickness_m,
                fields.sea_ice_thickness_m,
                fields.ocean_mask,
                fields.lake_mask,
                fields.river_mask,
                field_count
            );
        }
        return {
            .resolution = snapshot.resolution,
            .elevation = backing.elevation_km,
            .temperature = backing.temperature_celsius,
            .precipitation = backing.precipitation_m_year,
            .water_level = backing.water_level_km,
            .ocean_mask = fields.ocean_mask,
            .lake_mask = fields.lake_mask,
            .river_mask = fields.river_mask,
        };
    }
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
GenerationMapRasterSource rasterSource(
    generation::GlobalAtlas const& atlas,
    GenerationMapRasterBacking& backing
)
{
    if (atlas.configuration.model_tag != generation::TerrainModelTag::V21)
    {
        return rasterSource(atlas);
    }
    constexpr uint32_t SOURCE_RESOLUTION = 256U;
    constexpr size_t SOURCE_COUNT = static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION;
    constexpr std::array<uint32_t, 2U> SOURCE_SHAPE{SOURCE_RESOLUTION, SOURCE_RESOLUTION};
    constexpr std::array<uint32_t, 3U> SOURCE_PHASE_SHAPE{8U, SOURCE_RESOLUTION, SOURCE_RESOLUTION};
    std::vector<generation::V9RegisteredFieldView> const fields = generation::v21RegisteredFields(atlas);
    std::span<double const> const elevation = v21GridField<double>(
        fields, "elevation", generation::V9FieldType::Float64, SOURCE_SHAPE
    );
    std::span<float const> const temperature = v21GridField<float>(
        fields, "temperature_C", generation::V9FieldType::Float32, SOURCE_PHASE_SHAPE
    );
    std::span<float const> const precipitation = v21GridField<float>(
        fields, "precipitation_mm_year", generation::V9FieldType::Float32, SOURCE_PHASE_SHAPE
    );
    std::span<float const> const water_level = v21GridField<float>(
        fields, "water_level", generation::V9FieldType::Float32, SOURCE_SHAPE
    );
    std::span<uint8_t const> const ocean = v21GridField<uint8_t>(
        fields, "ocean", generation::V9FieldType::Bool, SOURCE_SHAPE
    );
    std::span<uint8_t const> const lake = v21GridField<uint8_t>(
        fields, "lake", generation::V9FieldType::Bool, SOURCE_SHAPE
    );
    std::span<uint8_t const> const river = v21GridField<uint8_t>(
        fields, "river", generation::V9FieldType::Bool, SOURCE_SHAPE
    );
    std::span<float const> const snow = v21GridField<float>(
        fields, "land_snow_water_equivalent_m", generation::V9FieldType::Float32, SOURCE_PHASE_SHAPE
    );
    std::span<float const> const glacier = v21GridField<float>(
        fields, "glacier_thickness_m", generation::V9FieldType::Float32, SOURCE_PHASE_SHAPE
    );
    std::span<float const> const sea_ice = v21GridField<float>(
        fields, "sea_ice_thickness_m", generation::V9FieldType::Float32, SOURCE_PHASE_SHAPE
    );
    if (elevation.empty() || temperature.empty() || precipitation.empty() || water_level.empty()
        || ocean.empty() || lake.empty() || river.empty() || snow.empty() || glacier.empty() || sea_ice.empty())
    {
        return {};
    }
    backing.elevation_km.reserve(SOURCE_COUNT);
    backing.water_level_km.reserve(SOURCE_COUNT);
    for (size_t index = 0U; index < SOURCE_COUNT; ++index)
    {
        backing.elevation_km.push_back(static_cast<float>(elevation[index] / 1'000.0));
        backing.water_level_km.push_back(
            water_level[index] / 0.34F / 1'000.0F
        );
    }
    backing.temperature_celsius = v21MeanPhaseField(temperature, SOURCE_COUNT);
    backing.ice_temperature_celsius.assign(temperature.begin(),
        temperature.begin() + static_cast<std::vector<float>::difference_type>(SOURCE_COUNT));
    backing.precipitation_m_year = v21MeanPhaseField(precipitation, SOURCE_COUNT);
    for (float& value : backing.precipitation_m_year)
    {
        value /= 1'000.0F;
    }
    backing.v21_ice_surface_kind = nativeStockIceSurfaceKind(
        snow, glacier, sea_ice, ocean, lake, river, SOURCE_COUNT
    );
    return {
        .resolution = SOURCE_RESOLUTION,
        .elevation = backing.elevation_km,
        .temperature = backing.temperature_celsius,
        .precipitation = backing.precipitation_m_year,
        .water_level = backing.water_level_km,
        .ocean_mask = ocean,
        .lake_mask = lake,
        .river_mask = river,
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
    float const progress,
    bool const water_surface
) noexcept
{
    if (surface_kind == GENERATION_MAP_ICE_SNOW)
    {
        return blendColor(color, { 243U, 246U, 240U, 255U }, 0.92F * progress);
    }
    if (surface_kind == GENERATION_MAP_ICE_WATER)
    {
        return water_surface
            ? blendColor(color, { 176U, 221U, 237U, 255U }, 0.90F * progress)
            : blendColor(color, { 208U, 225U, 223U, 255U }, 0.58F * progress);
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
        || (uses_climate
            && ((!source.temperature.empty() && !isFiniteField(source.temperature, pixel_count))
                || (!source.precipitation.empty() && !isFiniteField(source.precipitation, pixel_count))
                || (source.temperature.empty() && source.precipitation.empty())))
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
                    climateColor(
                        source.temperature.empty() ? 0.0F : source.temperature[source_index],
                        source.precipitation.empty() ? 0.0F : source.precipitation[source_index],
                        !source.temperature.empty(), !source.precipitation.empty()
                    ),
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
                    bool const water_surface = source.ocean_mask.empty() || source.lake_mask.empty()
                        || source.river_mask.empty()
                        || source.ocean_mask[source_index] != 0U || source.lake_mask[source_index] != 0U
                        || source.river_mask[source_index] != 0U;
                    color = frozenSurfaceColor(color, surface_kind, 1.0F, water_surface);
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
    if ((snapshot.v9 || snapshot.v21) && !generation::isValidGenerationSnapshot(snapshot))
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
        snapshot.v21
            ? std::span<uint8_t const>{backing.v21_ice_surface_kind}
            : (display_ice_surface_kind.empty() && snapshot.v9
            ? std::span<uint8_t const>{ snapshot.v9->ice_surface_kind }
                : display_ice_surface_kind),
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
    float intensity = 1.0F;
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
    float formation_height = 0.0F;
    float ice_temperature = 0.0F;
    float ice_age_distance = -1.0F;
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
    bool native_palette = false;
    bool weather_fields_required = false;
    bool hydrology_available = false;
    bool climate_available = false;
    float sea_level = 0.0F;
};

struct AnimationPlate final
{
    float center_x = 0.0F;
    float center_y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    float rotation = 0.0F;
};

struct PreparedAnimationMap final
{
    uint32_t resolution = GENERATION_MAP_ANIMATION_RESOLUTION;
    std::vector<AnimationCell> cells;
    std::vector<AnimationPlate> plates;
    std::vector<int32_t> nearest_water;
    std::vector<AnimationPath> wind_paths;
    std::vector<AnimationPath> current_paths;
    std::vector<AnimationRainDrop> rain_drops;
    float sea_level = 0.0F;
    float elevation_range = 1.0F;
    std::array<float, 2U> elevation_limits{};
    bool native_palette = false;
    bool weather_fields_required = false;
    float minimum_ice_temperature = 0.0F;
    float maximum_ice_temperature = 0.0F;
    float maximum_ice_age_distance = 0.0F;
    bool has_ice = false;
    bool ice_fields_available = false;
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
    double cosine_x = 0.0;
    double sine_x = 0.0;
    double cosine_y = 0.0;
    double sine_y = 0.0;
    double u = 0.0;
    double v = 0.0;
    uint32_t count = 0U;
    uint32_t index = 0U;
};

void smoothNativeFormation(PreparedAnimationMap& data)
{
    static constexpr std::array MEMBERS{&AnimationCell::strain};
    static constexpr int32_t RADIUS = 12;
    static constexpr float WINDOW = static_cast<float>(RADIUS * 2 + 1);
    uint32_t const side = data.resolution;
    auto const wrap = [side](int32_t value) {
        int32_t const size = static_cast<int32_t>(side);
        return static_cast<uint32_t>((value % size + size) % size);
    };
    std::vector<std::array<float, MEMBERS.size()>> horizontal(data.cells.size());
    // Soften boundary uplift without blending plate ownership or motion.
    for (uint32_t pass = 0U; pass < 3U; ++pass)
    {
        for (uint32_t y = 0U; y < side; ++y)
        {
            std::array<float, MEMBERS.size()> sums{};
            for (int32_t offset = -RADIUS; offset <= RADIUS; ++offset)
            {
                auto const& cell = data.cells[static_cast<size_t>(y) * side + wrap(offset)];
                for (size_t field = 0U; field < MEMBERS.size(); ++field) sums[field] += cell.*MEMBERS[field];
            }
            for (uint32_t x = 0U; x < side; ++x)
            {
                auto& output = horizontal[static_cast<size_t>(y) * side + x];
                auto const& add = data.cells[static_cast<size_t>(y) * side + wrap(static_cast<int32_t>(x) + RADIUS + 1)];
                auto const& remove = data.cells[static_cast<size_t>(y) * side + wrap(static_cast<int32_t>(x) - RADIUS)];
                for (size_t field = 0U; field < MEMBERS.size(); ++field)
                {
                    output[field] = sums[field] / WINDOW;
                    sums[field] += add.*MEMBERS[field] - remove.*MEMBERS[field];
                }
            }
        }
        for (uint32_t x = 0U; x < side; ++x)
        {
            std::array<float, MEMBERS.size()> sums{};
            for (int32_t offset = -RADIUS; offset <= RADIUS; ++offset)
            {
                auto const& input = horizontal[static_cast<size_t>(wrap(offset)) * side + x];
                for (size_t field = 0U; field < MEMBERS.size(); ++field) sums[field] += input[field];
            }
            for (uint32_t y = 0U; y < side; ++y)
            {
                auto& cell = data.cells[static_cast<size_t>(y) * side + x];
                auto const& add = horizontal[static_cast<size_t>(wrap(static_cast<int32_t>(y) + RADIUS + 1)) * side + x];
                auto const& remove = horizontal[static_cast<size_t>(wrap(static_cast<int32_t>(y) - RADIUS)) * side + x];
                for (size_t field = 0U; field < MEMBERS.size(); ++field)
                {
                    cell.*MEMBERS[field] = sums[field] / WINDOW;
                    sums[field] += add[field] - remove[field];
                }
            }
        }
    }
}

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
        && std::all_of(fields.ice_kind.begin(), fields.ice_kind.end(), [native = fields.native_palette](uint8_t const value) {
            return value <= GENERATION_MAP_ICE_SEA || (native && value == S2_MAP_PROJECTED_INLAND_ICE);
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
    uint32_t const columns = 6U;
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
                float const step_size = data.native_palette ? 1.6F : 0.8F;
                float const next_x = x + u / magnitude * step_size;
                float const next_y = y + v / magnitude * step_size;
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
    data.weather_fields_required = source.weather_fields_required;
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
    data.ice_fields_available = validDisplayIceFields(source, pixel_count);
    data.has_ice = data.ice_fields_available;

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
    data.native_palette = source.native_palette;
    data.elevation_limits = {minimum_elevation, maximum_elevation};
    data.elevation_range = std::max(0.001F, maximum_elevation - minimum_elevation);

    if (data.has_ice)
    {
        data.has_ice = std::any_of(data.cells.begin(), data.cells.end(), [](AnimationCell const& cell) {
            return cell.ice_kind != GENERATION_MAP_ICE_NONE;
        });
    }

    if (data.native_palette && data.has_ice && !source.ice_temperature.empty())
    {
        // The illustrative ice age expands from actual phase-zero ice stocks. It
        // never invents disconnected sheets from a coarse climate threshold.
        using FrontCell = std::pair<float, uint32_t>;
        std::priority_queue<FrontCell, std::vector<FrontCell>, std::greater<FrontCell>> front;
        for (uint32_t index = 0U; index < cell_count; ++index)
        {
            if (data.cells[index].ice_kind != GENERATION_MAP_ICE_NONE)
            {
                data.cells[index].ice_age_distance = 0.0F;
                front.emplace(0.0F, index);
            }
        }
        while (!front.empty())
        {
            auto const [distance, index] = front.top();
            front.pop();
            if (distance != data.cells[index].ice_age_distance)
            {
                continue;
            }
            data.maximum_ice_age_distance = std::max(data.maximum_ice_age_distance, distance);
            int32_t const x = static_cast<int32_t>(index % data.resolution);
            int32_t const y = static_cast<int32_t>(index / data.resolution);
            int32_t const side = static_cast<int32_t>(data.resolution);
            for (int32_t offset_y = -1; offset_y <= 1; ++offset_y)
            {
                for (int32_t offset_x = -1; offset_x <= 1; ++offset_x)
                {
                    if (offset_x == 0 && offset_y == 0)
                    {
                        continue;
                    }
                    uint32_t const neighbor = animationCellIndex(
                        static_cast<uint32_t>((x + offset_x + side) % side),
                        static_cast<uint32_t>((y + offset_y + side) % side));
                    AnimationCell& cell = data.cells[neighbor];
                    float const terrain_resistance = std::abs(cell.elevation - data.cells[index].elevation)
                        / data.elevation_range * 16.0F;
                    float const warmth_resistance = std::clamp(cell.ice_temperature / 10.0F, 0.0F, 1.0F);
                    float const next_distance = distance
                        + (offset_x != 0 && offset_y != 0 ? 1.414'213'56F : 1.0F)
                            * (1.0F + terrain_resistance + warmth_resistance * 2.0F);
                    if (cell.ice_temperature <= 10.0F
                        && (cell.ice_age_distance < 0.0F || next_distance < cell.ice_age_distance))
                    {
                        cell.ice_age_distance = next_distance;
                        front.emplace(next_distance, neighbor);
                    }
                }
            }
        }
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
                if (data.native_palette)
                {
                    double const phase_x = static_cast<double>(x) * 6.283'185'3 / data.resolution;
                    double const phase_y = static_cast<double>(y) * 6.283'185'3 / data.resolution;
                    plate.cosine_x += std::cos(phase_x);
                    plate.sine_x += std::sin(phase_x);
                    plate.cosine_y += std::cos(phase_y);
                    plate.sine_y += std::sin(phase_y);
                }
                ++plate.count;
                data.cells[cell_index].plate_index = static_cast<int16_t>(plate.index);
            }
        }
        plate_count = static_cast<uint32_t>(plates.size());
        std::vector<std::array<float, 2U>> plate_motion(plate_count);
        std::vector<std::array<float, 2U>> plate_centers(plate_count);
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
            auto center = [&](double const sine, double const cosine) {
                if (std::hypot(sine, cosine) < static_cast<double>(plate.count) * 1.0e-6)
                {
                    return static_cast<float>(data.resolution) * 0.5F;
                }
                float value = static_cast<float>(std::atan2(sine, cosine) * data.resolution / 6.283'185'3);
                return value < 0.0F ? value + static_cast<float>(data.resolution) : value;
            };
            plate_centers[plate.index] = {
                center(plate.sine_x, plate.cosine_x), center(plate.sine_y, plate.cosine_y),
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
            if (data.native_palette)
            {
                data.plates.resize(plate_count);
                for (uint32_t plate = 0U; plate < plate_count; ++plate)
                {
                    data.plates[plate] = {
                        .center_x = plate_centers[plate][0], .center_y = plate_centers[plate][1],
                        .u = plate_motion[plate][0], .v = plate_motion[plate][1],
                        .rotation = 0.28F * (plate_motion[plate][0] - plate_motion[plate][1] * 0.6F),
                    };
                }
            }
            for (uint32_t cell_index = 0U; cell_index < cell_count; ++cell_index)
            {
                AnimationCell& cell = data.cells[cell_index];
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

    if (data.native_palette)
    {
        smoothNativeFormation(data);
        for (uint32_t y = 0U; y < data.resolution; ++y)
        {
            for (uint32_t x = 0U; x < data.resolution; ++x)
            {
                AnimationCell& cell = data.cells[animationCellIndex(x, y)];
                float const wave_x = static_cast<float>(x) * 6.283'185'3F / static_cast<float>(data.resolution);
                float const wave_y = static_cast<float>(y) * 6.283'185'3F / static_cast<float>(data.resolution);
                cell.formation_height = (maximum_elevation - minimum_elevation)
                    * (0.10F * std::sin(wave_x * 3.0F + cell.plate_u) * std::cos(wave_y * 2.0F + cell.plate_v)
                        + 0.06F * cell.strain);
            }
        }
    }

    if (data.has_wind)
    {
        data.wind_paths = makeAnimationPaths(data, false, 36U);
    }
    if (data.has_currents)
    {
        data.current_paths = makeAnimationPaths(data, true, 36U);
    }
    if (data.has_precipitation)
    {
        // Each region keeps its own precipitation-weighted sample. A wetter
        // continent cannot consume the rain budget of every other wet region.
        auto hash = [](uint32_t value) {
            value ^= value >> 16U;
            value *= 0x7feb'352dU;
            value ^= value >> 15U;
            value *= 0x846c'a68bU;
            return value ^ (value >> 16U);
        };
        for (uint32_t y = 0U; y < data.resolution; y += 8U)
        {
            for (uint32_t x = 0U; x < data.resolution; x += 16U)
            {
                float best = std::numeric_limits<float>::max();
                uint32_t selected = 0U;
                float total = 0.0F;
                for (uint32_t dy = 0U; dy < 8U; ++dy)
                {
                    for (uint32_t dx = 0U; dx < 16U; ++dx)
                    {
                        uint32_t const index = animationCellIndex(x + dx, y + dy);
                        AnimationCell const& cell = data.cells[index];
                        if (cell.ice_kind != GENERATION_MAP_ICE_NONE
                            || (data.has_temperature && cell.temperature <= 0.0F)
                            || cell.precipitation <= 0.0F) continue;
                        float const weight = std::log1p(cell.precipitation);
                        total += weight;
                        float const uniform = (static_cast<float>(hash(index + 1U) & 0x00ff'ffffU) + 1.0F)
                            / 16'777'217.0F;
                        float const rank = -std::log(uniform) / weight;
                        if (rank < best) { best = rank; selected = index; }
                    }
                }
                if (total <= 0.0F) continue;
                float const density = std::clamp(total / (128.0F * std::log1p(0.5F)), 0.0F, 1.0F);
                float const inclusion = static_cast<float>(hash(y * data.resolution + x + 73U) & 0xffffU) / 65'535.0F;
                if (inclusion > density) continue;
                data.rain_drops.push_back({
                    .cell_index = selected,
                    .phase = static_cast<float>(hash(selected + 137U) & 0xffffU) / 65'535.0F,
                    .intensity = std::clamp(std::log1p(data.cells[selected].precipitation) / std::log1p(1.0F), 0.15F, 1.0F),
                });
            }
        }
    }
    return data;
}

// V10 has its own producer identity and metre datum; only the presentation
// representation below is shared with the other native models.
std::optional<PreparedAnimationMap> prepareV10AnimationMap(
    generation::V10GenerationSnapshotFields const& source, uint32_t resolution)
{
    constexpr size_t COUNT = 256U * 256U;
    if (resolution != 256U || source.height_raw.size() != COUNT || source.plate_ids.size() != COUNT)
        return std::nullopt;
    std::vector<float> elevation(COUNT), temperature, precipitation, water;
    std::vector<uint8_t> projected_ice;
    bool const canonical = source.source_elevation_m.size() == COUNT;
    for (size_t i = 0U; i < COUNT; ++i)
        elevation[i] = static_cast<float>(canonical ? source.source_elevation_m[i] / 1'000.0
            : static_cast<double>(source.height_raw[i]) / 1'000.0);
    bool const climate = source.local_effective_celsius.size() == 8U * 32U * 32U
        && source.theta_celsius.size() == 8U * 32U * 32U && source.ground_m.size() == 32U * 32U
        && source.phase_precipitation_mm_year.size() == 8U * COUNT && canonical;
    bool const hydrology = source.water_level_m.size() == COUNT
        && source.ocean_mask.size() == COUNT && source.lake_mask.size() == COUNT
        && source.river_mask.size() == COUNT;
    if (climate)
    {
        temperature.resize(COUNT); precipitation.resize(COUNT);
        if (hydrology)
        {
            projected_ice.resize(COUNT);
        }
        // Forcing is registered at coarse cell centres; heights are canonical
        // vertices. Keep the producer's local fine-surface lapse and ocean theta.
        auto sample = [](std::span<double const> values, uint32_t phase, uint32_t x, uint32_t y) {
            double const gx = static_cast<double>(x) / 8.0 - .5;
            double const gy = static_cast<double>(y) / 8.0 - .5;
            int32_t const ix = static_cast<int32_t>(std::floor(gx)), iy = static_cast<int32_t>(std::floor(gy));
            double const u = gx - ix, v = gy - iy;
            auto at = [&](int32_t xx, int32_t yy) {
                return values[(phase * 32U + static_cast<uint32_t>((yy + 32) % 32)) * 32U
                    + static_cast<uint32_t>((xx + 32) % 32)];
            };
            return at(ix, iy) * (1.0-u) * (1.0-v) + at(ix+1, iy) * u * (1.0-v)
                + at(ix, iy+1) * (1.0-u) * v + at(ix+1, iy+1) * u * v;
        };
        for (uint32_t y=0U; y<256U; ++y) for (uint32_t x=0U; x<256U; ++x)
        {
            size_t const i = y*256U+x;
            bool const ocean = hydrology && source.ocean_mask[i] != 0U;
            double const lapse = .0065 * (std::max(10.0 * source.source_elevation_m[i], 0.0)
                - sample(source.ground_m, 0U, x, y));
            double mean_temperature=0.0, mean_rain=0.0;
            std::array<double, 8U> seasonal_temperature{};
            for (uint32_t phase=0U; phase<8U; ++phase)
            {
                seasonal_temperature[phase] = ocean ? sample(source.theta_celsius, phase, x, y)
                    : sample(source.local_effective_celsius, phase, x, y) - lapse;
                mean_temperature += seasonal_temperature[phase];
                mean_rain += source.phase_precipitation_mm_year[phase*COUNT+i];
            }
            temperature[i]=static_cast<float>(mean_temperature/8.0);
            precipitation[i]=static_cast<float>(mean_rain/8'000.0);
            if (hydrology)
            {
                projected_ice[i] = s2ProjectedInlandIceKind(seasonal_temperature, ocean,
                    source.lake_mask[i] != 0U, source.river_mask[i] != 0U);
            }
        }
    }
    for (float level : source.water_level_m) water.push_back(level / 1'000.0F);
    std::vector<uint8_t> ice;
    if (hydrology && source.land_snow_water_equivalent_m.size() == 8U*COUNT
        && source.glacier_thickness_m.size() == 8U*COUNT && source.sea_ice_thickness_m.size() == 8U*COUNT)
    {
        // Keep phase-mean stocks, temperature and water masks in the shared V10 policy.
        auto const snow = v21MeanPhaseField<float>(source.land_snow_water_equivalent_m, COUNT);
        auto const glacier = v21MeanPhaseField<float>(source.glacier_thickness_m, COUNT);
        auto const sea_ice = v21MeanPhaseField<float>(source.sea_ice_thickness_m, COUNT);
        ice.resize(COUNT);
        for (size_t index = 0U; index < COUNT; ++index)
        {
            generation::V10SurfaceSample annual_mean;
            annual_mean.temperature_celsius = temperature.empty() ? 0.0 : temperature[index];
            annual_mean.snow_water_equivalent_m = snow[index];
            annual_mean.glacier_thickness_m = glacier[index];
            annual_mean.sea_ice_thickness_m = sea_ice[index];
            annual_mean.ocean = source.ocean_mask[index] != 0U;
            annual_mean.lake = source.lake_mask[index] != 0U;
            annual_mean.river = source.river_mask[index] != 0U;
            switch (generation::classifyV10AnnualMeanSurfaceCover(annual_mean))
            {
            case generation::V10SurfaceCover::None:
                ice[index] = GENERATION_MAP_ICE_NONE;
                break;
            case generation::V10SurfaceCover::Snow:
                ice[index] = GENERATION_MAP_ICE_SNOW;
                break;
            case generation::V10SurfaceCover::GlacierIce:
                ice[index] = GENERATION_MAP_ICE_GLACIER;
                break;
            case generation::V10SurfaceCover::SeaIce:
                ice[index] = GENERATION_MAP_ICE_SEA;
                break;
            }
        }
    }
    if (!projected_ice.empty())
    {
        if (ice.empty())
        {
            ice.assign(COUNT, GENERATION_MAP_ICE_NONE);
        }
        for (size_t i = 0U; i < COUNT; ++i)
        {
            if (ice[i] == GENERATION_MAP_ICE_NONE)
            {
                ice[i] = projected_ice[i];
            }
        }
    }
    std::vector<int16_t> compact(COUNT);
    std::vector<float> vx(COUNT), vy(COUNT);
    for (size_t i=0U; i<COUNT; ++i)
    {
        auto const direction=std::find_if(source.plate_directions.begin(),source.plate_directions.end(),
            [&](auto const& value){return value.plate_id==source.plate_ids[i];});
        if(direction==source.plate_directions.end()) return std::nullopt;
        compact[i]=static_cast<int16_t>(direction-source.plate_directions.begin());
        vx[i]=direction->x; vy[i]=direction->y;
    }
    return prepareAnimationMap(AnimationSourceFields{
        .resolution=resolution, .elevation=elevation, .temperature=temperature, .precipitation=precipitation,
        .water_level=water, .plate_vx=vx, .plate_vy=vy, .plate_id=compact,
        .ocean_mask=source.ocean_mask, .lake_mask=source.lake_mask, .river_mask=source.river_mask,
        .ice_kind=ice, .ice_temperature=temperature, .native_palette=true,
        .hydrology_available=hydrology, .climate_available=climate, .sea_level=0.0F});
}

[[nodiscard]]
std::optional<PreparedAnimationMap> prepareAnimationMap(
    generation::GlobalAtlas const& atlas,
    GenerationMapDisplayIceMaskHandle const& final_display_ice_mask
)
{
    if (atlas.configuration.model_tag == generation::TerrainModelTag::V10)
    {
        constexpr std::array<uint32_t, 2U> GRID{256U,256U}, COARSE{32U,32U};
        constexpr std::array<uint32_t, 3U> PHASES{8U,256U,256U}, CLIMATE{8U,32U,32U};
        auto const registered = generation::v10RegisteredFields(atlas);
        generation::V10GenerationSnapshotFields source;
        auto copy = [&]<typename T>(std::vector<T>& target, std::string_view name,
            generation::V9FieldType type, auto const& shape) {
            auto const field = v21GridField<T>(registered,name,type,shape);
            target.assign(field.begin(),field.end());
        };
        copy(source.height_raw,"height_raw",generation::V9FieldType::Float32,GRID);
        copy(source.source_elevation_m,"height_m",generation::V9FieldType::Float64,GRID);
        copy(source.local_effective_celsius,"fine_local_effective_C",generation::V9FieldType::Float64,CLIMATE);
        copy(source.theta_celsius,"fine_theta_C",generation::V9FieldType::Float64,CLIMATE);
        copy(source.ground_m,"fine_ground_m",generation::V9FieldType::Float64,COARSE);
        copy(source.phase_precipitation_mm_year,"fine_precipitation_mm_year",generation::V9FieldType::Float64,PHASES);
        copy(source.water_level_m,"water_level",generation::V9FieldType::Float32,GRID);
        copy(source.ocean_mask,"ocean",generation::V9FieldType::Bool,GRID);
        copy(source.lake_mask,"lake",generation::V9FieldType::Bool,GRID);
        copy(source.river_mask,"river",generation::V9FieldType::Bool,GRID);
        copy(source.land_snow_water_equivalent_m,"land_snow_water_equivalent_m",generation::V9FieldType::Float32,PHASES);
        copy(source.glacier_thickness_m,"glacier_thickness_m",generation::V9FieldType::Float32,PHASES);
        copy(source.sea_ice_thickness_m,"sea_ice_thickness_m",generation::V9FieldType::Float32,PHASES);
        auto const ids = generation::v10CurrentPlateIds(atlas);
        source.plate_ids.assign(ids.begin(),ids.end());
        auto const directions = generation::v10PlateDirections(atlas);
        source.plate_directions.assign(directions.begin(),directions.end());
        auto prepared = prepareV10AnimationMap(source,256U);
        if (prepared)
        {
            auto climateMean = [&](std::string_view name) {
                auto const field = v21GridField<double>(registered,name,generation::V9FieldType::Float64,CLIMATE);
                return field.empty() ? std::vector<float>{}
                    : expandV21ClimateField(v21MeanPhaseField(field,32U*32U));
            };
            auto const u=climateMean("u"), v=climateMean("v");
            auto const ocean_u=climateMean("ocean_u"), ocean_v=climateMean("ocean_v");
            prepared->has_wind=u.size()==256U*256U && v.size()==256U*256U;
            prepared->has_currents=ocean_u.size()==256U*256U && ocean_v.size()==256U*256U;
            for(size_t i=0U;i<prepared->cells.size();++i)
            {
                if(prepared->has_wind) {prepared->cells[i].wind_u=u[i];prepared->cells[i].wind_v=v[i];}
                if(prepared->has_currents) {prepared->cells[i].current_u=ocean_u[i];prepared->cells[i].current_v=ocean_v[i];}
            }
        }
        return prepared;
    }
    if (atlas.configuration.model_tag == generation::TerrainModelTag::V21)
    {
        static_cast<void>(final_display_ice_mask);
        constexpr uint32_t SOURCE_RESOLUTION = 256U;
        constexpr size_t SOURCE_COUNT = static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION;
        constexpr uint32_t CLIMATE_RESOLUTION = 32U;
        constexpr size_t CLIMATE_COUNT = static_cast<size_t>(CLIMATE_RESOLUTION) * CLIMATE_RESOLUTION;
        constexpr std::array<uint32_t, 3U> CLIMATE_PHASE_SHAPE{8U, CLIMATE_RESOLUTION, CLIMATE_RESOLUTION};
        GenerationMapRasterBacking backing;
        GenerationMapRasterSource const raster = rasterSource(atlas, backing);
        if (raster.resolution != SOURCE_RESOLUTION)
        {
            return std::nullopt;
        }
        std::vector<generation::V9RegisteredFieldView> const registered_fields =
            generation::v21RegisteredFields(atlas);
        auto const climateMean = [&](std::string_view const name) {
            std::span<double const> const field = v21GridField<double>(
                registered_fields, name, generation::V9FieldType::Float64, CLIMATE_PHASE_SHAPE
            );
            return field.empty()
                ? std::vector<float>{}
                : expandV21ClimateField(v21MeanPhaseField(field, CLIMATE_COUNT));
        };
        std::vector<float> const moisture = climateMean("bucket_kg_m2");
        std::vector<float> const wind_u = climateMean("u");
        std::vector<float> const wind_v = climateMean("v");
        std::vector<float> const current_u = climateMean("ocean_u");
        std::vector<float> const current_v = climateMean("ocean_v");
        std::vector<uint32_t> const plate_ids(generation::v21CurrentPlateIds(atlas).begin(),
            generation::v21CurrentPlateIds(atlas).end());
        std::vector<int16_t> compact_plate_ids;
        std::vector<float> plate_vx;
        std::vector<float> plate_vy;
        std::span<generation::V9PlateDirection const> const directions = generation::v21PlateDirections(atlas);
        if (plate_ids.size() == SOURCE_COUNT && directions.size() <= 64U)
        {
            std::vector<generation::V9PlateDirection> ordered_directions(directions.begin(), directions.end());
            std::sort(ordered_directions.begin(), ordered_directions.end(), [](auto const& left, auto const& right) {
                return left.plate_id < right.plate_id;
            });
            compact_plate_ids.resize(SOURCE_COUNT);
            plate_vx.resize(SOURCE_COUNT);
            plate_vy.resize(SOURCE_COUNT);
            for (size_t index = 0U; index < SOURCE_COUNT; ++index)
            {
                auto const direction = std::lower_bound(
                    ordered_directions.begin(), ordered_directions.end(), plate_ids[index],
                    [](generation::V9PlateDirection const& candidate, uint32_t const plate_id) {
                        return candidate.plate_id < plate_id;
                    }
                );
                if (direction == ordered_directions.end() || direction->plate_id != plate_ids[index])
                {
                    compact_plate_ids.clear();
                    plate_vx.clear();
                    plate_vy.clear();
                    break;
                }
                compact_plate_ids[index] = static_cast<int16_t>(direction - ordered_directions.begin());
                plate_vx[index] = direction->x;
                plate_vy[index] = direction->y;
            }
        }
        AnimationSourceFields fields{
            .resolution = SOURCE_RESOLUTION,
            .elevation = raster.elevation,
            .temperature = raster.temperature,
            .precipitation = raster.precipitation,
            .water_level = raster.water_level,
            .moisture = moisture,
            .wind_u = wind_u,
            .wind_v = wind_v,
            .current_u = current_u,
            .current_v = current_v,
            .plate_vx = plate_vx,
            .plate_vy = plate_vy,
            .plate_id = compact_plate_ids,
            .ocean_mask = raster.ocean_mask,
            .lake_mask = raster.lake_mask,
            .river_mask = raster.river_mask,
            .ice_kind = backing.v21_ice_surface_kind,
            .ice_temperature = backing.ice_temperature_celsius,
            .native_palette = true,
            .weather_fields_required = true,
            .hydrology_available = hasValidHydrology(raster, SOURCE_COUNT),
            .climate_available = hasValidClimate(raster, SOURCE_COUNT),
            .sea_level = 0.0F,
        };
        return prepareAnimationMap(fields);
    }
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
    if (snapshot.v10)
    {
        if (!generation::isValidGenerationSnapshot(snapshot)) return std::nullopt;
        return prepareV10AnimationMap(*snapshot.v10, snapshot.resolution);
    }
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
            .wind_u = source.wind_u,
            .wind_v = source.wind_v,
            .current_u = source.current_u,
            .current_v = source.current_v,
            .plate_vx = plate_vx,
            .plate_vy = plate_vy,
            .plate_id = compact_plate_ids,
            .ocean_mask = source.ocean_mask,
            .lake_mask = source.lake_mask,
            .river_mask = source.river_mask,
            .ice_kind = source.ice_surface_kind,
            .ice_temperature = source.temperature_celsius,
            .native_palette = true,
            .hydrology_available = snapshot.stage >= generation::GenerationStage::V9Water,
            .climate_available = !source.temperature_celsius.empty() || !source.precipitation_mm_year.empty(),
            .sea_level = 0.0F,
        };
        static_cast<void>(final_display_ice_mask);
        return prepareAnimationMap(fields);
    }

    if (snapshot.v21)
    {
        if (!generation::isValidGenerationSnapshot(snapshot))
        {
            return std::nullopt;
        }
        static_cast<void>(final_display_ice_mask);
        generation::V21GenerationSnapshotFields const& source = *snapshot.v21;
        GenerationMapRasterBacking backing;
        GenerationMapRasterSource const raster = rasterSource(snapshot, backing);
        uint32_t const pixel_count = snapshot.resolution * snapshot.resolution;
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
                    directions.begin(), directions.end(), source.plate_ids[index],
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
            .elevation = raster.elevation,
            .temperature = raster.temperature,
            .precipitation = raster.precipitation,
            .water_level = raster.water_level,
            .wind_u = source.wind_u,
            .wind_v = source.wind_v,
            .current_u = source.current_u,
            .current_v = source.current_v,
            .plate_vx = plate_vx,
            .plate_vy = plate_vy,
            .plate_id = compact_plate_ids,
            .ocean_mask = source.ocean_mask,
            .lake_mask = source.lake_mask,
            .river_mask = source.river_mask,
            .ice_kind = backing.v21_ice_surface_kind,
            .ice_temperature = backing.ice_temperature_celsius,
            .native_palette = true,
            .weather_fields_required = true,
            .hydrology_available = hasValidHydrology(raster, pixel_count),
            .climate_available = snapshot.stage >= generation::GenerationStage::V21Climate
                && isFiniteField(raster.temperature, pixel_count)
                && (raster.precipitation.empty() || isFiniteField(raster.precipitation, pixel_count)),
            .sea_level = 0.0F,
        };
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
std::chrono::milliseconds presentationDuration(std::chrono::milliseconds const baseline) noexcept
{
    int64_t const duration = std::max<int64_t>(baseline.count(), 1);
    return std::chrono::milliseconds{duration + duration / 2};
}

[[nodiscard]]
GenerationMapAnimationPhase animationPhaseForIndex(uint32_t const phase_index) noexcept
{
    static constexpr std::array<GenerationMapAnimationPhase, 5U> PHASES{
        GenerationMapAnimationPhase::Tectonics,
        GenerationMapAnimationPhase::Water,
        GenerationMapAnimationPhase::Relief,
        GenerationMapAnimationPhase::Ice,
        GenerationMapAnimationPhase::Climate,
    };
    return phase_index < PHASES.size() ? PHASES[phase_index] : GenerationMapAnimationPhase::Arrival;
}

[[nodiscard]]
AnimationPhaseState animationPhase(GenerationMapAnimationClock const& clock, bool const native_pacing = false) noexcept
{
    int64_t const elapsed = std::max<int64_t>(clock.visible_elapsed.count(), 0);
    int64_t const duration = presentationDuration(clock.formation_duration).count();
    if (elapsed >= duration)
    {
        return {.phase = GenerationMapAnimationPhase::Arrival, .phase_index = 5U, .progress = 1.0F};
    }
    auto const& phase_weights = native_pacing ? NATIVE_ANIMATION_PHASE_WEIGHTS : ANIMATION_PHASE_WEIGHTS;
    int64_t const weighted_elapsed = elapsed * 100;
    int64_t phase_start = 0;
    uint32_t phase_index = 0U;
    for (; phase_index < static_cast<uint32_t>(phase_weights.size()); ++phase_index)
    {
        int64_t const phase_end = phase_start + duration * phase_weights[phase_index];
        if (weighted_elapsed < phase_end)
        {
            break;
        }
        phase_start = phase_end;
    }
    int64_t const phase_length = duration * phase_weights[phase_index];
    float const progress = static_cast<float>(weighted_elapsed - phase_start)
        / static_cast<float>(phase_length);
    return {
        .phase = animationPhaseForIndex(phase_index),
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
    if (phase.phase_index == 2U)
    {
        return 2.4F * std::sin(progress * 3.141'592'65F);
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
        return 0.93F;
    }
    if (phase.phase_index == 2U)
    {
        return 0.93F + progress * 0.07F;
    }
    return 1.0F;
}

[[nodiscard]]
float animationSeaLevel(PreparedAnimationMap const& data, AnimationPhaseState const& phase, float const progress) noexcept
{
    if (data.native_palette && phase.phase_index < 3U)
    {
        float const floor = std::min(data.elevation_limits[0], 0.0F);
        float const dry_floor = floor - 0.01F;
        return phase.phase_index == 0U ? data.sea_level + dry_floor
            : (phase.phase_index == 1U ? data.sea_level + dry_floor * (1.0F - progress) : data.sea_level);
    }
    if (phase.phase_index == 0U)
    {
        return data.sea_level - 0.3F + progress * 0.1F;
    }
    if (phase.phase_index == 1U)
    {
        return data.sea_level - 0.2F + progress * 0.2F;
    }
    return data.sea_level;
}

[[nodiscard]]
float animationWaterProgress(AnimationPhaseState const& phase, float const progress) noexcept
{
    if (phase.phase_index < 1U)
    {
        return 0.0F;
    }
    if (phase.phase_index == 1U)
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
    return phase.phase_index >= 4U ? progress : 0.0F;
}

[[nodiscard]]
float animationUpliftProgress(AnimationPhaseState const& phase, float const progress, bool const has_motion) noexcept
{
    if (!has_motion)
    {
        return 0.0F;
    }
    if (phase.phase_index == 2U)
    {
        return std::sin(progress * 3.141'592'65F) * 0.8F;
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
    if ((kind == 3U && source.ice_kind != GENERATION_MAP_ICE_NONE)
        || (kind == GENERATION_MAP_ICE_WATER && (source.water_mask & 3U) == 0U)
        || (kind == 3U && source.wetness < 0.22F))
    {
        return;
    }
    uint32_t const render_scale = std::max(1U, image.width / data.resolution);
    uint32_t const render_x = std::min(image.width - 1U, static_cast<uint32_t>(std::lround(x * render_scale)));
    uint32_t const render_y = std::min(image.height - 1U, static_cast<uint32_t>(std::lround(y * render_scale)));
    if (data.native_palette)
    {
        uint32_t const left = source_x * render_scale;
        uint32_t const top = source_y * render_scale;
        for (uint32_t dy = 0U; dy < render_scale; ++dy)
        {
            for (uint32_t dx = 0U; dx < render_scale; ++dx)
            {
                blendPixel(image.rgba[(top + dy) * image.width + left + dx], color, alpha);
            }
        }
        return;
    }
    GenerationMapRgba8& pixel = image.rgba[
        render_y * image.width + render_x
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
    uint32_t const render_scale = std::max(1U, image.width / data.resolution);
    uint32_t const steps = std::max(
        1U,
        static_cast<uint32_t>(std::ceil(distance * static_cast<float>(render_scale)))
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

[[nodiscard]]
float periodicDelta(float const value, float const side) noexcept
{
    return value > side * 0.5F ? value - side : (value < -side * 0.5F ? value + side : value);
}

[[nodiscard]]
AnimationPoint platePoint(
    AnimationPlate const& plate, AnimationPoint const source, float const drift, float const formation,
    float const side
) noexcept
{
    float const angle = plate.rotation * formation;
    float const cosine = std::cos(angle);
    float const sine = std::sin(angle);
    float const dx = periodicDelta(source.x - plate.center_x, side);
    float const dy = periodicDelta(source.y - plate.center_y, side);
    return {.x = plate.center_x + cosine * dx - sine * dy - plate.u * drift,
        .y = plate.center_y + sine * dx + cosine * dy - plate.v * drift};
}

void animateWeather(
    GenerationMapImage& image,
    PreparedAnimationMap const& data,
    float const progress,
    bool const show_wind = true,
    bool const show_currents = true,
    bool const show_rain = true,
    float const opacity_scale = 1.0F,
    float const rain_drift = 0.0F,
    float const rain_formation = 0.0F,
    float const glyph_scale = 1.0F
)
{
    float const opacity = std::clamp(progress * 5.0F, 0.0F, 1.0F)
        * std::clamp(opacity_scale, 0.0F, 1.0F);
    if (opacity <= 0.0F)
    {
        return;
    }
    auto drawPaths = [&](std::vector<AnimationPath> const& paths, GenerationMapRgba8 const& color, uint8_t kind) {
        uint32_t const render_scale = std::max(1U, image.width / data.resolution);
        float const unit_scale = glyph_scale / static_cast<float>(render_scale);
        auto isWaterNeighborhood = [&](AnimationPoint const& center, float const radius) {
            int32_t const center_x = static_cast<int32_t>(center.x);
            int32_t const center_y = static_cast<int32_t>(center.y);
            int32_t const cell_radius = static_cast<int32_t>(std::ceil(radius));
            for (int32_t offset_y = -cell_radius; offset_y <= cell_radius; ++offset_y)
            {
                for (int32_t offset_x = -cell_radius; offset_x <= cell_radius; ++offset_x)
                {
                    if (static_cast<float>(offset_x * offset_x + offset_y * offset_y) > radius * radius)
                    {
                        continue;
                    }
                    int32_t const source_x = center_x + offset_x;
                    int32_t const source_y = center_y + offset_y;
                    if (source_x < 0 || source_y < 0 || source_x >= static_cast<int32_t>(data.resolution)
                        || source_y >= static_cast<int32_t>(data.resolution))
                    {
                        return false;
                    }
                    AnimationCell const& cell = data.cells[animationCellIndex(
                        static_cast<uint32_t>(source_x), static_cast<uint32_t>(source_y)
                    )];
                    if ((cell.water_mask & 3U) == 0U)
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        for (AnimationPath const& path : paths)
        {
            float const path_length = static_cast<float>(path.points.size() - 1U);
            float const head = std::fmod(progress * 26.0F + path.phase, path_length);
            auto pointAt = [&path, path_length](float position) {
                float const wrapped = std::fmod(std::max(position, 0.0F), path_length);
                uint32_t const index = static_cast<uint32_t>(wrapped);
                uint32_t const next = std::min(static_cast<uint32_t>(path.points.size() - 1U), index + 1U);
                float const fraction = wrapped - static_cast<float>(index);
                AnimationPoint const& from = path.points[index];
                AnimationPoint const& to = path.points[next];
                return AnimationPoint{
                    .x = from.x + (to.x - from.x) * fraction,
                    .y = from.y + (to.y - from.y) * fraction,
                };
            };
            AnimationPoint const center = pointAt(head);
            uint32_t const direction_segment = std::min(
                static_cast<uint32_t>(head), static_cast<uint32_t>(path.points.size() - 2U)
            );
            AnimationPoint const& direction_from = path.points[direction_segment];
            AnimationPoint const& direction_to = path.points[direction_segment + 1U];
            float const direction_x = direction_to.x - direction_from.x;
            float const direction_y = direction_to.y - direction_from.y;
            float const magnitude = std::hypot(direction_x, direction_y);
            if (magnitude <= 1.0e-6F)
            {
                continue;
            }
            float const unit_x = direction_x / magnitude;
            float const unit_y = direction_y / magnitude;
            AnimationPoint const tip{
                .x = center.x + unit_x * 6.0F * unit_scale,
                .y = center.y + unit_y * 6.0F * unit_scale,
            };
            AnimationPoint const tail{
                .x = center.x - unit_x * 10.0F * unit_scale,
                .y = center.y - unit_y * 10.0F * unit_scale,
            };
            AnimationPoint const particle = pointAt(std::fmod(head + path_length * 0.45F, path_length));
            float const glyph_radius = 11.0F * unit_scale;
            float const particle_radius = 2.3F * unit_scale;
            if (kind == GENERATION_MAP_ICE_WATER
                && (!isWaterNeighborhood(center, glyph_radius)
                    || !isWaterNeighborhood(particle, particle_radius)))
            {
                continue;
            }
            GenerationMapRgba8 const outline{22U, 47U, 61U, 255U};
            auto drawOutlinedLine = [&](AnimationPoint const& from, AnimationPoint const& to) {
                float const normal_x = -unit_y * 0.55F * unit_scale;
                float const normal_y = unit_x * 0.55F * unit_scale;
                for (float const side : {-1.0F, 1.0F})
                {
                    animationLine(
                        image,
                        data,
                        {from.x + normal_x * side, from.y + normal_y * side},
                        {to.x + normal_x * side, to.y + normal_y * side},
                        outline,
                        0.92F * opacity,
                        kind
                    );
                }
                animationLine(image, data, from, to, color, opacity, kind);
            };
            drawOutlinedLine(tail, tip);
            for (float const side : {-1.0F, 1.0F})
            {
                AnimationPoint const wing{
                    .x = tip.x - unit_x * 4.0F * unit_scale - unit_y * side * 2.2F * unit_scale,
                    .y = tip.y - unit_y * 4.0F * unit_scale + unit_x * side * 2.2F * unit_scale,
                };
                drawOutlinedLine(tip, wing);
            }
            for (int32_t offset_y = -3; offset_y <= 3; ++offset_y)
            {
                for (int32_t offset_x = -3; offset_x <= 3; ++offset_x)
                {
                    if (static_cast<float>(offset_x * offset_x + offset_y * offset_y) <= 2.3F * 2.3F)
                    {
                        animationPlot(
                            image,
                            data,
                            particle.x + static_cast<float>(offset_x) * unit_scale,
                            particle.y + static_cast<float>(offset_y) * unit_scale,
                            color,
                            0.9F * opacity,
                            kind
                        );
                    }
                }
            }
        }
    };
    if (show_wind)
    {
        drawPaths(data.wind_paths, { 62U, 232U, 216U, 255U }, 0U);
    }
    if (show_currents)
    {
        drawPaths(data.current_paths, { 162U, 224U, 249U, 255U }, GENERATION_MAP_ICE_WATER);
    }
    for (AnimationRainDrop const& drop : data.rain_drops)
    {
        if (!show_rain)
        {
            break;
        }
        AnimationCell const& cell = data.cells[drop.cell_index];
        float const fraction = std::fmod(progress * (2.5F + drop.intensity * 2.0F) + drop.phase, 1.0F);
        float const magnitude = std::hypot(cell.wind_u, cell.wind_v);
        float const wind_x = magnitude > 1.0e-8F ? cell.wind_u / magnitude : 0.0F;
        float const wind_y = magnitude > 1.0e-8F ? cell.wind_v / magnitude : 0.0F;
        auto wrap = [&data](float const coordinate) {
            float const side = static_cast<float>(data.resolution);
            float const wrapped = std::fmod(coordinate, side);
            return wrapped < 0.0F ? wrapped + side : wrapped;
        };
        AnimationPoint center{static_cast<float>(drop.cell_index % data.resolution),
            static_cast<float>(drop.cell_index / data.resolution)};
        if (data.native_palette && cell.plate_index >= 0 && !data.plates.empty())
        {
            center = platePoint(data.plates[static_cast<size_t>(cell.plate_index)], center,
                rain_drift, rain_formation, static_cast<float>(data.resolution));
        }
        float const x = wrap(center.x + wind_x * fraction * 1.5F);
        float const y = wrap(center.y + wind_y * fraction * 1.5F + fraction * 6.0F);
        animationLine(
            image,
            data,
            { x, y },
            { x + wind_x * 1.5F, y + wind_y * 1.5F + 3.0F },
            { 176U, 211U, 228U, 255U },
            (0.35F + 0.5F * drop.intensity) * opacity,
            data.native_palette ? 0U : 3U
        );
    }
}

void reconstructRifts(
    PreparedAnimationMap const& data, std::span<uint8_t const> covered,
    std::vector<float>& heights, std::vector<int32_t>& sources, std::vector<uint8_t>& reconstructed,
    uint32_t const scale, float const drift
)
{
    if (covered.empty() || std::ranges::all_of(covered, [](uint8_t value) { return value != 0U; })) return;
    uint32_t const side = data.resolution;
    uint32_t const count = side * side;
    uint32_t const raster_side = side * scale;
    auto rasterIndex = [=](uint32_t i) { return (i / side) * scale * raster_side + (i % side) * scale; };
    auto neighbors = [=](uint32_t i) {
        uint32_t const x = i % side, y = i / side;
        return std::array<uint32_t, 4U>{y * side + (x + side - 1U) % side,
            y * side + (x + 1U) % side, ((y + side - 1U) % side) * side + x,
            ((y + 1U) % side) * side + x};
    };
    std::vector<float> original(count), field(count), next(count);
    std::vector<int32_t> nearest(count, -1), water(count, -1), distance(count, -1);
    std::vector<uint8_t> contact(count, 0U);
    std::vector<uint32_t> queue;
    for (uint32_t i = 0U; i < count; ++i)
    {
        original[i] = heights[rasterIndex(i)];
        if (covered[i] != 0U) { nearest[i] = static_cast<int32_t>(i); distance[i] = 0; queue.push_back(i); }
        else contact[i] = 1U;
    }
    for (size_t head = 0U; head < queue.size(); ++head)
    {
        uint32_t const i = queue[head];
        for (uint32_t n : neighbors(i)) if (nearest[n] < 0)
        {
            nearest[n] = nearest[i]; distance[n] = distance[i] + 1; queue.push_back(n);
        }
    }
    queue.clear();
    for (uint32_t i = 0U; i < count; ++i)
    {
        auto const& cell = data.cells[static_cast<size_t>(sources[rasterIndex(i)])];
        if (covered[i] != 0U && ((cell.water_mask & 1U) != 0U || cell.elevation <= data.sea_level))
        { water[i] = static_cast<int32_t>(i); queue.push_back(i); }
        field[i] = nearest[i] >= 0 ? original[static_cast<size_t>(nearest[i])] : original[i];
    }
    for (size_t head = 0U; head < queue.size(); ++head)
    {
        uint32_t const i = queue[head];
        for (uint32_t n : neighbors(i)) if (water[n] < 0)
        { water[n] = water[i]; queue.push_back(n); }
    }
    // Only the contact strip deforms; rigid plate interiors remain exact.
    for (uint32_t pass = 0U; pass < 2U; ++pass)
    {
        auto expanded = contact;
        for (uint32_t i = 0U; i < count; ++i) if (contact[i] != 0U)
            for (uint32_t n : neighbors(i)) expanded[n] = 1U;
        contact = std::move(expanded);
    }
    // Interpolate adjacent moved boundary heights before lowering the rift centre.
    for (uint32_t pass = 0U; pass < 12U; ++pass)
    {
        next = field;
        for (uint32_t i = 0U; i < count; ++i) if (contact[i] != 0U)
        {
            float total = 0.0F;
            for (uint32_t n : neighbors(i)) total += field[n];
            next[i] = total * 0.25F;
        }
        field.swap(next);
    }
    for (uint32_t i = 0U; i < count; ++i) if (covered[i] == 0U && water[i] >= 0)
    {
        uint32_t const wet = static_cast<uint32_t>(water[i]);
        float const d = static_cast<float>(distance[i]);
        float const depth = smoothProgress(d / (d + 4.0F));
        field[i] += (std::min(original[wet], data.sea_level) - field[i]) * depth;
        sources[rasterIndex(i)] = sources[rasterIndex(wet)];
    }
    // A short scalar relaxation joins shallow coasts to the deeper local floor.
    for (uint32_t pass = 0U; pass < 4U; ++pass)
    {
        next = field;
        for (uint32_t i = 0U; i < count; ++i) if (contact[i] != 0U)
        {
            float total = field[i] * 4.0F;
            for (uint32_t n : neighbors(i)) total += field[n];
            next[i] = total * 0.125F;
        }
        field.swap(next);
    }
    float const amount = smoothProgress(std::min(1.0F, drift * 0.5F));
    for (uint32_t i = 0U; i < count; ++i) if (contact[i] != 0U)
    {
        float const height = std::lerp(original[i], field[i], amount);
        int32_t const source = sources[rasterIndex(i)];
        for (uint32_t oy = 0U; oy < scale; ++oy) for (uint32_t ox = 0U; ox < scale; ++ox)
        {
            uint32_t const target = rasterIndex(i) + oy * raster_side + ox;
            heights[target] = height; sources[target] = source; reconstructed[target] = 1U;
        }
    }
}

// A smooth display-only convection scalar perturbs the same height and thermal
// surface. It has no ridge thresholds or separate molten color image.
[[nodiscard]]
std::vector<float> surfaceConvection(float const seconds)
{
    auto noise = [](float x, float y, int32_t period, uint32_t salt) {
        x *= static_cast<float>(period);
        y *= static_cast<float>(period);
        int32_t const ix = static_cast<int32_t>(std::floor(x));
        int32_t const iy = static_cast<int32_t>(std::floor(y));
        float const fx = smoothProgress(x - std::floor(x));
        float const fy = smoothProgress(y - std::floor(y));
        auto value = [=](int32_t px, int32_t py) {
            px = (px % period + period) % period;
            py = (py % period + period) % period;
            uint32_t hash = static_cast<uint32_t>(px) * 0x9e37'79b9U
                ^ static_cast<uint32_t>(py) * 0x85eb'ca6bU ^ salt;
            hash = (hash ^ (hash >> 16U)) * 0x7feb'352dU;
            hash = (hash ^ (hash >> 15U)) * 0x846c'a68bU;
            return static_cast<float>((hash ^ (hash >> 16U)) & 0xffffU) / 65'535.0F;
        };
        return std::lerp(std::lerp(value(ix, iy), value(ix + 1, iy), fx),
            std::lerp(value(ix, iy + 1), value(ix + 1, iy + 1), fx), fy);
    };
    static constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    static std::vector<AnimationPoint> const FLOW = [noise] {
        std::vector<AnimationPoint> flow(SIDE * SIDE);
        for (uint32_t y = 0U; y < SIDE; ++y)
            for (uint32_t x = 0U; x < SIDE; ++x)
            {
                float const nx = static_cast<float>(x) / static_cast<float>(SIDE);
                float const ny = static_cast<float>(y) / static_cast<float>(SIDE);
                flow[y * SIDE + x] = {
                    (noise(nx, ny + 0.015F, 4, 2027U) - noise(nx, ny - 0.015F, 4, 2027U)) * 4.0F,
                    -(noise(nx + 0.015F, ny, 4, 2027U) - noise(nx - 0.015F, ny, 4, 2027U)) * 4.0F,
                };
            }
        return flow;
    }();
    float const travel = std::sin(seconds * 0.5F) * 0.60F;
    std::vector<float> surface(SIDE * SIDE);
    for (uint32_t y = 0U; y < SIDE; ++y)
    {
        for (uint32_t x = 0U; x < SIDE; ++x)
        {
            float const nx = static_cast<float>(x) / static_cast<float>(SIDE);
            float const ny = static_cast<float>(y) / static_cast<float>(SIDE);
            AnimationPoint const velocity = FLOW[y * SIDE + x];
            float const px = nx - travel * velocity.x;
            float const py = ny - travel * velocity.y;
            surface[y * SIDE + x] = 2.0F * (noise(px, py, 8, 791U) * 0.4F
                + noise(px, py, 32, 159U) * 0.45F + noise(px, py, 64, 947U) * 0.15F) - 1.0F;
        }
    }
    return surface;
}

[[nodiscard]]
GenerationMapImage renderAnimationFrame(
    PreparedAnimationMap const& data,
    AnimationPhaseState const& phase,
    bool const reduced_motion,
    GenerationMapImage const* const final_image,
    float const visible_seconds = 0.0F
)
{
    float const progress = phaseProgressForRender(phase, reduced_motion);
    // Preserve the world-space drift authored on the prototype's 128-cell presentation grid.
    float const drift = data.native_palette && data.has_motion
        ? (phase.phase_index < 2U ? 24.0F
            : (phase.phase_index == 2U ? 24.0F * (1.0F - progress) : 0.0F))
        : animationDrift(phase, progress, data.has_motion) * 2.0F;
    float const height_scale = data.native_palette && phase.phase_index == 0U
        ? 0.04F + 0.89F * progress : animationHeightScale(phase, progress);
    float const sea_level = animationSeaLevel(data, phase, progress);
    float const water_progress = animationWaterProgress(phase, progress);
    float const ice_progress = animationIceProgress(phase, progress);
    float const climate_progress = animationClimateProgress(phase, progress);
    float const uplift = animationUpliftProgress(phase, progress, data.has_motion);
    bool const samples_fractional_drift = data.has_motion && drift > 0.0F;
    uint32_t const render_resolution = GENERATION_MAP_ANIMATION_RENDER_RESOLUTION;
    uint32_t const render_count = render_resolution * render_resolution;
    uint32_t const raster_scale = GENERATION_MAP_ANIMATION_SCALE;
    uint32_t const raster_resolution = render_resolution;
    uint32_t const raster_count = raster_resolution * raster_resolution;
    std::vector<int32_t> raster_source_map(raster_count, -1);
    std::vector<uint8_t> native_covered(data.native_palette ? data.cells.size() : 0U, 1U);
    std::vector<uint8_t> reconstructed(data.native_palette ? raster_count : 0U, 0U);
    std::vector<float> height_map(raster_count, -std::numeric_limits<float>::infinity());
    std::vector<uint8_t> raster_fallback_map(samples_fractional_drift ? raster_count : 0U, 0U);
    std::vector<GenerationMapRgba8> motion_colors;
    std::vector<GenerationMapRgba8> motion_fallback_colors;
    std::vector<float> source_slopes;
    std::vector<float> const convection = data.native_palette && phase.phase_index == 0U
        ? surfaceConvection(visible_seconds) : std::vector<float>{};
    float const formation = data.native_palette
        ? (phase.phase_index == 0U ? 0.02F + progress * 0.53F
            : (phase.phase_index == 1U ? 0.55F
                : (phase.phase_index == 2U ? 0.55F * (1.0F - smoothProgress(progress)) : 0.0F))) : 0.0F;
    auto const formed_height = [&](uint32_t const x, uint32_t const y) {
        AnimationCell const& cell = data.cells[animationCellIndex(x, y)];
        return cell.elevation * height_scale + cell.strain * uplift + formation * cell.formation_height;
    };
    if (samples_fractional_drift && !data.native_palette)
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
                GenerationMapRgba8 color = elevationColor(cell.elevation);
                if (data.has_hydrology && water_progress > 0.0F && cell.water_mask != 0U)
                {
                    color = waterColor(
                        color,
                        cell.elevation,
                        cell.water_level,
                        (cell.water_mask & 1U) != 0U ? 1U : 0U,
                        (cell.water_mask & 2U) != 0U ? 1U : 0U,
                        (cell.water_mask & 4U) != 0U ? 1U : 0U,
                        water_progress
                    );
                }
                float const fallback_height = std::min(cell.elevation, sea_level - 0.2F);
                motion_colors[index] = color;
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
    std::vector<std::array<float, 2U>> plate_rotations;
    for (auto const& plate : data.plates)
    {
        plate_rotations.push_back({std::cos(plate.rotation * formation), std::sin(plate.rotation * formation)});
    }
    for (uint32_t y = 0U; y < data.resolution; ++y)
    {
        for (uint32_t x = 0U; x < data.resolution; ++x)
        {
            uint32_t const source_index = animationCellIndex(x, y);
            AnimationCell const& cell = data.cells[source_index];
            if (data.native_palette)
            {
                auto const wrapped = [&data](float const coordinate) {
                    int32_t const side = static_cast<int32_t>(data.resolution);
                    return static_cast<uint32_t>((static_cast<int32_t>(std::lround(coordinate)) % side + side) % side);
                };
                uint32_t sampled = source_index;
                if (!data.plates.empty() && (drift > 0.0F || formation > 0.0F))
                {
                    float highest = -std::numeric_limits<float>::infinity();
                    float const side = static_cast<float>(data.resolution);
                    for (size_t plate_index = 0U; plate_index < data.plates.size(); ++plate_index)
                    {
                        auto const& plate = data.plates[plate_index];
                        auto const& rotation = plate_rotations[plate_index];
                        float const dx = periodicDelta(static_cast<float>(x) - plate.center_x + plate.u * drift, side);
                        float const dy = periodicDelta(static_cast<float>(y) - plate.center_y + plate.v * drift, side);
                        uint32_t const sample_x = wrapped(plate.center_x + rotation[0] * dx + rotation[1] * dy);
                        uint32_t const sample_y = wrapped(plate.center_y - rotation[1] * dx + rotation[0] * dy);
                        uint32_t const candidate = animationCellIndex(sample_x, sample_y);
                        if (data.cells[candidate].plate_index != static_cast<int16_t>(plate_index))
                        {
                            continue;
                        }
                        float const height = formed_height(sample_x, sample_y);
                        if (height > highest)
                        {
                            highest = height;
                            sampled = candidate;
                        }
                    }
                    native_covered[source_index] = std::isfinite(highest) ? 1U : 0U;
                }
                uint32_t const sample_x = sampled % data.resolution;
                uint32_t const sample_y = sampled / data.resolution;
                // Each candidate retains its source plate; separation exposes the existing underlay.
                for (uint32_t offset_y = 0U; offset_y < raster_scale; ++offset_y)
                {
                    for (uint32_t offset_x = 0U; offset_x < raster_scale; ++offset_x)
                    {
                        uint32_t const target = (y * raster_scale + offset_y) * raster_resolution
                            + x * raster_scale + offset_x;
                        raster_source_map[target] = static_cast<int32_t>(sampled);
                        height_map[target] = formed_height(sample_x, sample_y);
                    }
                }
                continue;
            }
            float const height = formed_height(x, y);
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

    if (data.native_palette)
    {
        reconstructRifts(data, native_covered, height_map, raster_source_map, reconstructed, raster_scale, drift);
        if (!convection.empty())
        {
            float const amplitude = (data.elevation_limits[1] - data.elevation_limits[0])
                * 0.035F * (1.0F - progress);
            for (uint32_t index = 0U; index < raster_count; ++index)
                height_map[index] += amplitude * convection[(index / raster_resolution / raster_scale)
                    * data.resolution + (index % raster_resolution / raster_scale)];
        }
    }
    GenerationMapImage image;
    image.width = render_resolution;
    image.height = render_resolution;
    image.rgba.resize(render_count);
    bool const can_blend_final_image = final_image != nullptr
        && final_image->width == image.width && final_image->height == image.height
        && final_image->rgba.size() == image.rgba.size();
    for (uint32_t output_index = 0U; output_index < render_count; ++output_index)
    {
        uint32_t const x = output_index % render_resolution;
        uint32_t const y = output_index / render_resolution;
        uint32_t const raster_x = x;
        uint32_t const raster_y = y;
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
        if (samples_fractional_drift && !data.native_palette)
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
                    color = frozenSurfaceColor(
                        color, native_cell.ice_kind, 1.0F, native_cell.water_mask != 0U
                    );
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
        if (data.native_palette)
        {
            float const ice_age_progress = phase.phase_index < 3U ? 0.0F
                : (phase.phase_index == 3U ? phase.progress : 1.0F);
            float const ice_age_strength = ice_age_progress <= 0.5F
                ? ice_age_progress * 2.0F : (1.0F - ice_age_progress) * 2.0F;
            float const settled_ice_strength = ice_progress <= 0.5F
                ? 0.0F : smoothProgress((ice_progress - 0.5F) * 2.0F);
            uint8_t const settled_ice_kind = settled_ice_strength >= 1.0F
                ? native_cell.ice_kind : GENERATION_MAP_ICE_NONE;
            uint8_t const canonical_ice_kind = settled_ice_kind == GENERATION_MAP_ICE_WATER
                    && (native_cell.water_mask & 1U) != 0U
                ? 3U : settled_ice_kind;
            uint8_t const age_ice_kind = (source_cell.water_mask & 7U) != 0U ? 3U : 2U;
            bool const canonical_wet = reconstructed[raster_index] != 0U
                ? elevation <= source_cell.water_level
                : (data.has_hydrology ? source_cell.water_mask != 0U : (elevation <= sea_level || wet));
            float const visible_elevation = canonical_wet || water_progress >= 1.0F
                ? elevation : std::max(0.0F, elevation - sea_level);
            auto const canonical = s2NativeTerrainColor(
                visible_elevation * 1'000.0F,
                {data.elevation_limits[0] * 1'000.0F, data.elevation_limits[1] * 1'000.0F},
                {height_map[east] * 1'000.0F, height_map[south] * 1'000.0F}, canonical_wet,
                (source_cell.water_level - elevation) * 1'000.0F, canonical_ice_kind);
            for (uint32_t channel = 0U; channel < 3U; ++channel)
            {
                color[channel] = static_cast<uint8_t>(std::clamp(std::lround(canonical[channel]), 0L, 255L));
            }
            float const biome_reveal = phase.phase_index < 3U ? 0.0F
                : (phase.phase_index == 3U ? 0.4F * settled_ice_strength
                    : 0.4F + 0.6F * climate_progress);
            if (can_blend_final_image && biome_reveal > 0.0F)
            {
                color = blendColor(color, final_image->rgba[output_index], biome_reveal);
            }
            if (ice_age_strength > 0.0F && data.has_ice)
            {
                // Interpolate only the display front and its temperature edge;
                // native stocks and all generation fields retain their exact cells.
                float const fraction_x = static_cast<float>(raster_x % raster_scale) / raster_scale;
                float const fraction_y = static_cast<float>(raster_y % raster_scale) / raster_scale;
                auto interpolate = [&](auto const& value) {
                    auto const& east_cell = data.cells[animationCellIndex((native_x + 1U) % data.resolution, native_y)];
                    auto const& south_cell = data.cells[animationCellIndex(native_x, (native_y + 1U) % data.resolution)];
                    auto const& diagonal_cell = data.cells[animationCellIndex(
                        (native_x + 1U) % data.resolution, (native_y + 1U) % data.resolution)];
                    return std::lerp(std::lerp(value(native_cell), value(east_cell), fraction_x),
                        std::lerp(value(south_cell), value(diagonal_cell), fraction_x), fraction_y);
                };
                float const distance = interpolate([&](AnimationCell const& cell) {
                    return cell.ice_age_distance >= 0.0F
                        ? cell.ice_age_distance : data.maximum_ice_age_distance + 12.0F;
                });
                float const temperature = interpolate([](AnimationCell const& cell) { return cell.ice_temperature; });
                float const stock = interpolate([](AnimationCell const& cell) {
                    return cell.ice_kind != GENERATION_MAP_ICE_NONE ? 1.0F : 0.0F;
                });
                float const eligible = std::max(stock,
                    smoothProgress(std::clamp((10.0F - temperature) / 10.0F, 0.0F, 1.0F)));
                float const front = ice_age_strength * (data.maximum_ice_age_distance + 12.0F);
                float const coverage = eligible * smoothProgress(ice_age_strength)
                    * smoothProgress(std::clamp((front - distance) / 18.0F, 0.0F, 1.0F));
                auto const ice_material = s2NativeTerrainColor(
                    visible_elevation * 1'000.0F,
                    {data.elevation_limits[0] * 1'000.0F, data.elevation_limits[1] * 1'000.0F},
                    {height_map[east] * 1'000.0F, height_map[south] * 1'000.0F}, canonical_wet,
                    (source_cell.water_level - elevation) * 1'000.0F, age_ice_kind);
                float const relief_shade = canonical_wet ? 1.0F
                    : std::clamp(1.0F - slope / data.elevation_range * 3.0F, 0.68F, 1.04F);
                GenerationMapRgba8 ice_color{0U, 0U, 0U, 255U};
                for (uint32_t channel = 0U; channel < 3U; ++channel)
                {
                    ice_color[channel] = static_cast<uint8_t>(std::clamp(
                        std::lround(ice_material[channel] * relief_shade), 0L, 255L));
                }
                float const thickness = canonical_wet
                    ? 0.30F + 0.25F * std::clamp(-temperature / 20.0F, 0.0F, 1.0F)
                    : 0.55F + 0.25F * std::clamp((elevation - data.elevation_limits[0]) / data.elevation_range, 0.0F, 1.0F);
                color = blendColor(color, ice_color, coverage * thickness);
            }
            if (settled_ice_strength > 0.0F && settled_ice_strength < 1.0F
                && native_cell.ice_kind != GENERATION_MAP_ICE_NONE)
            {
                color = frozenSurfaceColor(
                    color, native_cell.ice_kind, settled_ice_strength, native_cell.water_mask != 0U
                );
            }
            float const temperature_overlay_strength = phase.phase_index >= 3U
                ? (ice_progress <= 0.5F
                    ? 0.02F + ice_age_strength * 0.06F
                    : ice_age_strength * 0.08F)
                : 0.0F;
            if (temperature_overlay_strength > 0.0F)
            {
                float const temperature_mix = ice_progress <= 0.5F
                    ? smoothProgress(ice_progress * 2.0F)
                    : 1.0F - smoothProgress((ice_progress - 0.5F) * 2.0F);
                float const displayed_temperature = 12.0F
                    + (native_cell.temperature - 12.0F) * temperature_mix;
                color = blendColor(
                    color,
                    rampColor(displayed_temperature, TEMPERATURE_COLORS),
                    temperature_overlay_strength
                );
            }
            float const crust_relief = std::clamp(0.65F
                + (elevation - data.elevation_limits[0]) / data.elevation_range * 0.55F
                - slope / data.elevation_range * 2.0F, 0.45F, 1.2F);
            GenerationMapRgba8 volcanic_crust = COOLING_WATER_CRUST_COLOR;
            if (!canonical_wet)
            {
                for (uint32_t channel = 0U; channel < 3U; ++channel)
                    volcanic_crust[channel] = static_cast<uint8_t>(std::lround(volcanic_crust[channel] * crust_relief));
            }
            if (phase.phase_index == 0U)
            {
                static constexpr std::array<MapColorStop, 5U> THERMAL_MATERIAL{{
                    {0.0F, COOLING_WATER_CRUST_COLOR},
                    {0.18F, {98U, 48U, 30U, 255U}},
                    {0.42F, {158U, 65U, 20U, 255U}},
                    {0.72F, {220U, 92U, 15U, 255U}},
                    {1.0F, {255U, 178U, 55U, 255U}},
                }};
                float const circulation = convection[native_index];
                float const rising_relief = std::clamp((elevation - data.sea_level) / data.elevation_range, 0.0F, 1.0F);
                float const temperature = std::clamp((0.85F + circulation * 0.20F) * (1.0F - progress * progress)
                    - progress * rising_relief * 1.5F, 0.0F, 1.0F);
                float const roughness = smoothProgress(std::clamp((0.65F - temperature) / 0.65F, 0.0F, 1.0F));
                color = rampColor(temperature, THERMAL_MATERIAL);
                float const material_relief = canonical_wet ? 1.0F : std::lerp(1.0F, crust_relief, roughness);
                for (uint32_t channel = 0U; channel < 3U; ++channel)
                    color[channel] = static_cast<uint8_t>(std::lround(color[channel] * material_relief));
            }
            if (data.native_palette && phase.phase_index == 1U)
            {
                color = blendColor(volcanic_crust, color, water_progress);
            }
            image.rgba[output_index] = color;
            image.rgba[output_index][3U] = 255U;
            continue;
        }
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
        }
        image.rgba[output_index] = color;
        image.rgba[output_index][3U] = 255U;
    }
    if (!data.native_palette && phase.phase_index == 4U && can_blend_final_image)
    {
        for (size_t index = 0U; index < image.rgba.size(); ++index)
        {
            image.rgba[index] = blendColor(image.rgba[index], final_image->rgba[index], progress);
        }
    }
    bool const rain_visible = phase.phase_index == 1U && !data.rain_drops.empty();
    bool const flows_visible = phase.phase_index == 4U || (data.native_palette && phase.phase_index == 3U);
    if ((rain_visible || flows_visible) && (data.has_wind || data.has_currents || !data.rain_drops.empty()))
    {
        float weather_progress = phaseProgressForRender(phase, reduced_motion);
        float weather_opacity_scale = 1.0F;
        if (rain_visible)
        {
            weather_progress = phase.progress * 3.0F;
            weather_opacity_scale = 1.0F - smoothProgress(
                std::clamp((phase.progress - 0.8F) / 0.2F, 0.0F, 1.0F)
            );
        }
        if (data.native_palette && flows_visible)
        {
            // Real climate vectors remain visible through ice growth and final weather.
            weather_progress = phase.phase_index == 3U ? 0.2F + weather_progress * 0.3F
                : 0.5F + weather_progress * 0.5F;
        }
        animateWeather(
            image, data, weather_progress, flows_visible, flows_visible, rain_visible,
            weather_opacity_scale, data.native_palette ? drift : 0.0F, formation
        );
    }
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
    std::span<uint8_t const> const display_ice_surface_kind,
    bool const include_climate_tint
)
{
    GenerationMapRasterBacking backing;
    GenerationMapRasterSource const source = rasterSource(atlas, backing);
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
        atlas.configuration.model_tag == generation::TerrainModelTag::V21
            ? std::span<uint8_t const>{backing.v21_ice_surface_kind}
            : display_ice_surface_kind,
        {},
        RasterOptions{
            .output_resolution = source.resolution,
            .climate_progress = include_climate_tint ? 1.0F : 0.0F,
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
    uint32_t rendered_phase_index = 0U;
    uint32_t rendered_tick = 0U;
    GenerationMapImageHandle rendered_image;
    GenerationMapImageHandle transition_image;
    GenerationMapImageHandle final_image;
    std::optional<uint32_t> transition_phase_index;
    float transition_start_progress = 0.0F;
    float transition_duration = 0.1F;
    GenerationMapImageHandle source_transition_image;
    std::chrono::milliseconds source_transition_started{};
    uint32_t rendered_source_transition_tick = 0U;
    std::chrono::milliseconds presentation_elapsed{};
    std::chrono::milliseconds last_visible_elapsed{};
    uint32_t timing_samples = 0U;
    uint32_t timing_calls = 0U;
};

struct GenerationMapUpdateTiming final
{
    GenerationMapAnimationCache& cache;
    GenerationMapAnimationFrame const& frame;
    bool enabled = false;
    std::chrono::steady_clock::time_point started{};

    GenerationMapUpdateTiming(GenerationMapAnimationCache& cache_value, GenerationMapAnimationFrame const& frame_value)
        : cache{cache_value}, frame{frame_value}
    {
        char const* const requested = std::getenv("MC_DIAGNOSTIC_GENERATION_MAP_TIMING");
        enabled = requested != nullptr && std::string_view{requested} == "1" && cache.timing_samples < 2'048U;
        if (enabled)
        {
            started = std::chrono::steady_clock::now();
        }
    }

    ~GenerationMapUpdateTiming()
    {
        if (enabled)
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            ++cache.timing_calls;
            if (elapsed < 500 && cache.timing_calls % 64U != 0U)
            {
                return;
            }
            ++cache.timing_samples;
            std::clog << "[generation-map-cost] phase=" << static_cast<unsigned>(frame.phase)
                << " progress=" << frame.phase_progress << " update_us=" << elapsed
                << " plates=" << (cache.prepared ? cache.prepared->plates.size() : 0U) << '\n';
        }
    }
};


struct GenerationMapNativeCache final
{
    std::shared_ptr<generation::GlobalAtlas const> atlas;
    std::shared_ptr<generation::GenerationSnapshot const> snapshot;
    std::shared_ptr<S2MapImage const> source;
    std::optional<PreparedAnimationMap> prepared;
    GenerationHistoryFrame fields;
    std::vector<double> height, from_height, current_height, water;
    std::vector<double> basin_floor, basin_stage;
    // Potential follows visible geometry; accumulated cooling never runs backward.
    std::vector<float> cooling_potential, cooling_progress, cooling_exposure, cooling_pool;
    std::vector<float> cooling_contact, from_cooling_contact, target_cooling_contact;
    float cooling_elapsed_seconds = 0.0F;
    std::optional<float> meaningful_cooling_start_seconds;
    std::vector<uint32_t> plates;
    std::vector<uint8_t> biomes, ice, weather_ocean;
    std::vector<float> wind_u, wind_v, current_u, current_v, precipitation;
    float minimum_temperature = 0.0F;
    float ice_age_temperature = 0.0F;
    float cooling_duration_seconds = 8.0F;
    std::chrono::milliseconds origin{}, last_elapsed{}, adoption_elapsed{}, hydrology_elapsed{};
    uint64_t tick = std::numeric_limits<uint64_t>::max();
    bool started = false;
    bool reduced_motion = false;
    float progress = 0.0F;
    GenerationMapImageHandle image, surface;
};

namespace
{
constexpr float SLOW_STAGE_PACING = 4.0F / 3.0F;

float nativeEase(float value) { return smoothProgress(std::clamp(value, 0.0F, 1.0F)); }
float nativeRockGrain(float x, float y, bool fine = false, float warp = 1.0F)
{
    auto hash = [](uint32_t x, uint32_t y) {
        uint32_t h = x * 0x9e37'79b9U ^ y * 0x85eb'ca6bU ^ 0x68bc'21ebU;
        h = (h ^ (h >> 16U)) * 0x7feb'352dU;
        h = (h ^ (h >> 15U)) * 0x846c'a68bU;
        h ^= h >> 16U;
        return static_cast<float>(h & 65'535U) / 65'535.0F;
    };
    constexpr float SIDE = static_cast<float>(GENERATION_MAP_ANIMATION_RESOLUTION);
    x = std::fmod(x, SIDE); y = std::fmod(y, SIDE);
    if (x < 0.0F)
    {
        x += SIDE;
    }
    if (y < 0.0F)
    {
        y += SIDE;
    }
    // Integer wave directions keep the domain periodic while breaking the lattice axes.
    float const angle = 2.0F * std::numbers::pi_v<float> / SIDE;
    float warped_x = x + 7.0F * std::sin((2.0F * x + 3.0F * y) * angle)
        + 5.0F * std::sin((5.0F * x - 2.0F * y) * angle);
    float warped_y = y + 7.0F * std::sin((3.0F * x - 2.0F * y) * angle)
        + 5.0F * std::cos((2.0F * x + 5.0F * y) * angle);
    if (warp != 1.0F)
    {
        warped_x = x + warp * (warped_x - x);
        warped_y = y + warp * (warped_y - y);
    }
    x = std::fmod(warped_x + SIDE, SIDE);
    y = std::fmod(warped_y + SIDE, SIDE);
    auto noise = [&](uint32_t period) {
        float const xx = x * static_cast<float>(period) / SIDE;
        float const yy = y * static_cast<float>(period) / SIDE;
        uint32_t const ix = static_cast<uint32_t>(xx), iy = static_cast<uint32_t>(yy);
        float const fx = nativeEase(xx - ix), fy = nativeEase(yy - iy);
        auto tile = [&](uint32_t xx, uint32_t yy) { return hash(xx % period, yy % period); };
        return std::lerp(std::lerp(tile(ix, iy), tile(ix + 1U, iy), fx),
            std::lerp(tile(ix, iy + 1U), tile(ix + 1U, iy + 1U), fx), fy);
    };
    return noise(fine ? 43U : 11U) * .55F + noise(fine ? 79U : 29U) * .30F
        + noise(fine ? 113U : 61U) * .15F;
}
// Regenerate with script/generate_lava_crust_distance.cpp.
constexpr uint16_t NATIVE_CRUST_DISTANCE[] = {
// DONT_CHECK INCLUDE_ORDER
#include "../../assets/textures/terrain/lava-crust-distance.inc"
};
static_assert(std::span{NATIVE_CRUST_DISTANCE}.size()
    == GENERATION_MAP_ANIMATION_RESOLUTION * GENERATION_MAP_ANIMATION_RESOLUTION);

struct NativeLavaMaterial final
{
    std::array<float, 3U> crust{};
    std::array<float, 3U> molten{};
    float crust_weathering = 1.0F;
    float exposed_melt = 0.0F;
};

NativeLavaMaterial nativeLavaMaterial(float x, float y, float cooling, float seconds, uint32_t index)
{
    if (cooling >= 1.0F)
    {
        return {};
    }
    float const broad = nativeRockGrain(x, y, false, 0.3F);
    float const skin_start = nativeEase((cooling - 0.07F) / 0.12F);
    float signed_gap = 0.27F;
    if (skin_start > 0.0F)
    {
        // The baked periodic distance field uses the stationary material coordinates.
        float const spread = 1.59F * nativeEase((cooling - 0.08F) / 0.37F)
            + 1.23F * nativeEase((cooling - 0.51F) / 0.34F);
        signed_gap = static_cast<float>(NATIVE_CRUST_DISTANCE[index]) * (3.07F / 65'535.0F) - spread;
    }
    float const opening = nativeEase((signed_gap + 0.02F) / 0.27F);
    float const exposed_melt = std::lerp(1.0F, opening, skin_start)
        * (1.0F - nativeEase((cooling - 0.73F) / 0.20F));
    float vein = 0.0F;
    if (cooling > 0.27F && cooling < 0.87F)
    {
        // The gaps between cooling fronts lie on ridges of the distance field.
        constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
        uint32_t const row = index / SIDE;
        uint32_t const column = index % SIDE;
        auto distance = [&](uint32_t row_offset, uint32_t column_offset) {
            return static_cast<float>(NATIVE_CRUST_DISTANCE[
                ((row + row_offset) % SIDE) * SIDE + (column + column_offset) % SIDE]);
        };
        float const center = distance(0U, 0U);
        auto ridge = [&](float a, float b) {
            float const drop = center - std::min(a, b);
            float const rise = std::max(a, b) - center;
            return nativeEase((drop - std::max(0.0F, rise) - 350.0F) / 2400.0F)
                * (1.0F - nativeEase((rise - 150.0F) / 950.0F));
        };
        float const line = std::max({
            ridge(distance(0U, SIDE - 1U), distance(0U, 1U)),
            ridge(distance(SIDE - 1U, 0U), distance(1U, 0U)),
            ridge(distance(SIDE - 1U, SIDE - 1U), distance(1U, 1U)),
            ridge(distance(SIDE - 1U, 1U), distance(1U, SIDE - 1U)),
        });
        float const lifetime = nativeEase((cooling - 0.27F) / 0.19F)
            * (1.0F - nativeEase((cooling - 0.61F) / 0.26F));
        vein = 0.76F * line * lifetime * nativeEase((center - 18'000.0F) / 12'000.0F);
    }

    // Fine incandescent material continues moving through the exposed-fissure
    // lifetime. Crust and its gaps stay attached to the world as flicker fades.
    float const flow = 1.0F - nativeEase((cooling - 0.16F) / 0.77F);
    float const phase = 1.90F * seconds + broad * 14.0F;
    float const molten_grain = nativeRockGrain(x + flow * 0.9F * std::sin(phase),
        y + flow * 0.7F * std::cos(phase), true, 0.3F);
    float const hot_patches = nativeEase((molten_grain - 0.56F) / 0.26F);
    float const fluctuation = 8.0F * std::sin(phase) * flow;
    float const maturity = nativeEase((cooling - 0.23F) / 0.60F);
    float const initial_incandescence = 52.0F * (1.0F - nativeEase(cooling / 0.23F));
    float const raised_skin = 1.0F - nativeEase((-signed_gap - 0.02F) / 0.38F);
    float const grit = nativeRockGrain(x, y, true, 0.3F);
    return {
        {20.0F + 14.0F * broad + 7.0F * grit + 7.0F * raised_skin,
            16.0F + 11.0F * broad + 5.0F * grit + 3.0F * raised_skin,
            15.0F + 9.0F * broad + 4.0F * grit},
        {std::lerp(186.0F, 150.0F, maturity) + initial_incandescence + 24.0F * hot_patches + fluctuation,
            std::lerp(56.0F, 28.0F, maturity) + 33.0F * hot_patches + fluctuation,
            8.0F + 10.0F * hot_patches},
        nativeEase((cooling - 0.80F) / 0.20F),
        std::max(exposed_melt, vein),
    };
}

float advanceNativeProgress(float progress, float cap, float elapsed)
{
    // Integrate across pacing boundaries so a long frame cannot skip the slower
    // water filling and ice invasion. Retreat and the modern endpoint keep their pace.
    while (elapsed > 0.0F && progress < cap)
    {
        float const boundary = progress < .30F ? .30F : progress < .68F ? .68F : 1.0F;
        float const pacing = progress >= .30F && progress < .68F ? SLOW_STAGE_PACING : 1.0F;
        float const limit = std::min(boundary, cap);
        float const until_boundary = limit < cap || cap == 1.0F
            ? pacing * (cap == 1.0F ? limit - progress : -cap * std::log((cap - limit) / (cap - progress)))
            : std::numeric_limits<float>::infinity();
        if (elapsed < until_boundary)
        {
            return cap == 1.0F ? progress + elapsed / pacing
                : progress + (cap - progress) * -std::expm1(-elapsed / (pacing * cap));
        }
        progress = limit;
        elapsed -= until_boundary;
    }
    return progress;
}

// Approximate radiance from the thermal field only. Terrain RGB is never blurred.
void nativeThermalLight(GenerationMapImage& image, std::span<float const> energy)
{
    constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    if (std::none_of(energy.begin(), energy.end(), [](float value) { return value > 0.0F; }))
    {
        return;
    }
    auto spread = [&](int32_t radius) {
        std::vector<float> horizontal(energy.size()), result(energy.size());
        int32_t const side = static_cast<int32_t>(SIDE);
        float const width = static_cast<float>(radius * 2 + 1);
        for (int32_t y = 0; y < side; ++y)
        {
            float sum = 0.0F;
            for (int32_t dx = -radius; dx <= radius; ++dx) sum += energy[static_cast<size_t>(y * side + (dx + side) % side)];
            for (int32_t x = 0; x < side; ++x)
            {
                horizontal[static_cast<size_t>(y * side + x)] = sum / width;
                sum += energy[static_cast<size_t>(y * side + (x + radius + 1) % side)]
                    - energy[static_cast<size_t>(y * side + (x - radius + side) % side)];
            }
        }
        for (int32_t x = 0; x < side; ++x)
        {
            float sum = 0.0F;
            for (int32_t dy = -radius; dy <= radius; ++dy) sum += horizontal[static_cast<size_t>(((dy + side) % side) * side + x)];
            for (int32_t y = 0; y < side; ++y)
            {
                result[static_cast<size_t>(y * side + x)] = std::max(0.0F, sum / width);
                sum += horizontal[static_cast<size_t>(((y + radius + 1) % side) * side + x)]
                    - horizontal[static_cast<size_t>(((y - radius + side) % side) * side + x)];
            }
        }
        return result;
    };
    auto const near_glow = spread(2), far_glow = spread(6);
    constexpr std::array<float, 3U> LIGHT{100.0F, 38.0F, 7.0F};
    for (uint32_t y = 0U; y < SIDE; ++y) for (uint32_t x = 0U; x < SIDE; ++x)
    {
        float const glow = near_glow[y * SIDE + x] * .85F + far_glow[y * SIDE + x] * .30F;
        for (uint32_t oy = 0U; oy < 2U; ++oy) for (uint32_t ox = 0U; ox < 2U; ++ox)
        {
            auto& pixel = image.rgba[(y * 2U + oy) * image.width + x * 2U + ox];
            for (uint32_t c = 0U; c < 3U; ++c)
            {
                long const limit = c == 1U ? 254L : 255L;
                pixel[c] = static_cast<uint8_t>(std::clamp(std::lround(pixel[c] + glow * LIGHT[c]), 0L, limit));
            }
        }
    }
}
float nativeWrap(float value)
{
    constexpr float SIDE = static_cast<float>(GENERATION_MAP_ANIMATION_RESOLUTION);
    float const wrapped = value - std::floor(value / SIDE) * SIDE;
    // Adding a tiny negative remainder to SIDE may round back to SIDE.
    // Keep the periodic coordinate inside the grid's exclusive upper bound.
    return wrapped == SIDE ? 0.0F : wrapped;
}

double nativeSample(std::span<double const> values, float x, float y)
{
    constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    x = nativeWrap(x); y = nativeWrap(y);
    uint32_t const ix = static_cast<uint32_t>(x), iy = static_cast<uint32_t>(y);
    double const fx = x - ix, fy = y - iy;
    return std::lerp(std::lerp(values[iy * SIDE + ix], values[iy * SIDE + (ix + 1U) % SIDE], fx),
        std::lerp(values[((iy + 1U) % SIDE) * SIDE + ix],
            values[((iy + 1U) % SIDE) * SIDE + (ix + 1U) % SIDE], fx), fy);
}

// Adopt scalar source fields from the current surface, never a previous RGB image.
void adoptNativeStory(GenerationMapNativeCache& cache, PreparedAnimationMap prepared,
    std::span<double const> heights, std::span<uint32_t const> plates, bool cell_centres)
{
    constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    constexpr size_t COUNT = static_cast<size_t>(SIDE) * SIDE;
    cache.from_height = cache.current_height;
    cache.height.assign(heights.begin(), heights.end());
    cache.plates.assign(plates.begin(), plates.end());
    if (cache.plates.size() != COUNT) cache.plates.assign(COUNT, 0U);
    if (cell_centres)
    {
        auto const centred = cache.height;
        auto const centred_cells = prepared.cells;
        auto const centred_plates = cache.plates;
        for (uint32_t y = 0U; y < SIDE; ++y) for (uint32_t x = 0U; x < SIDE; ++x)
        {
            uint32_t const i = y * SIDE + x;
            cache.height[i] = nativeSample(centred, static_cast<float>(x) - .5F, static_cast<float>(y) - .5F);
            uint32_t const j = ((y + SIDE - 1U) % SIDE) * SIDE + (x + SIDE - 1U) % SIDE;
            cache.plates[i] = centred_plates[j];
            prepared.cells[i].plate_u = centred_cells[j].plate_u;
            prepared.cells[i].plate_v = centred_cells[j].plate_v;
        }
    }
    cache.water.resize(COUNT); cache.ice.resize(COUNT);
    // Zero is a real snowy biome, never a missing-field default.
    cache.biomes.assign(COUNT,255U);
    if(cache.snapshot && cache.snapshot->v21 && cache.snapshot->v21->biome_id.size()==COUNT)
        cache.biomes=cache.snapshot->v21->biome_id;
    for (size_t i = 0U; i < COUNT; ++i)
    {
        cache.water[i] = prepared.cells[i].water_level * 1'000.0;
        cache.ice[i] = prepared.cells[i].ice_kind;
    }
    cache.basin_floor = cache.height;
    cache.basin_stage = cache.water;
    double ocean_floor = std::numeric_limits<double>::infinity();
    double ocean_stage = std::numeric_limits<double>::infinity();
    for (size_t i = 0U; i < COUNT; ++i)
    {
        if ((prepared.cells[i].water_mask & 1U) != 0U)
        {
            ocean_floor = std::min(ocean_floor, cache.height[i]);
            ocean_stage = std::min(ocean_stage, cache.water[i]);
        }
    }
    std::span<int32_t const> lake_ids;
    if (cache.atlas)
    {
        auto const registry = cache.atlas->configuration.model_tag == generation::TerrainModelTag::V10
            ? generation::v10RegisteredFields(*cache.atlas)
            : cache.atlas->configuration.model_tag == generation::TerrainModelTag::V21
                ? generation::v21RegisteredFields(*cache.atlas) : generation::v9RegisteredFields(*cache.atlas);
        constexpr std::array<uint32_t, 2U> SHAPE{SIDE, SIDE};
        lake_ids = v21GridField<int32_t>(registry, "lake_id", generation::V9FieldType::Int32, SHAPE);
    }
    std::map<int32_t, std::pair<double, double>> lake_limits;
    if (lake_ids.size() == COUNT)
    {
        for (size_t i = 0U; i < COUNT; ++i)
        {
            if ((prepared.cells[i].water_mask & 3U) != 2U)
            {
                continue;
            }
            auto [entry, inserted] = lake_limits.try_emplace(lake_ids[i], cache.height[i], cache.water[i]);
            if (!inserted)
            {
                entry->second.first = std::min(entry->second.first, cache.height[i]);
                entry->second.second = std::min(entry->second.second, cache.water[i]);
            }
        }
    }
    std::vector<uint8_t> visited(COUNT, 0U);
    for (uint32_t i = 0U; i < COUNT; ++i)
    {
        uint8_t const mask = prepared.cells[i].water_mask;
        if ((mask & 1U) != 0U)
        {
            cache.basin_floor[i] = ocean_floor;
            cache.basin_stage[i] = ocean_stage;
        }
        else if ((mask & 2U) != 0U && lake_ids.size() == COUNT)
        {
            cache.basin_floor[i] = lake_limits.at(lake_ids[i]).first;
            cache.basin_stage[i] = lake_limits.at(lake_ids[i]).second;
        }
        else if ((mask & 2U) != 0U && !visited[i])
        {
            // Snapshot hydrology has no lake IDs. Connected actual lake cells
            // define deterministic basins; rivers do not merge distinct lakes.
            std::vector<uint32_t> basin{i};
            visited[i] = 1U;
            double floor = cache.height[i], stage = cache.water[i];
            for (size_t cursor = 0U; cursor < basin.size(); ++cursor)
            {
                uint32_t const cell = basin[cursor], x = cell % SIDE, y = cell / SIDE;
                floor = std::min(floor, cache.height[cell]);
                stage = std::min(stage, cache.water[cell]);
                for (uint32_t next : {y * SIDE + (x + 1U) % SIDE, y * SIDE + (x + SIDE - 1U) % SIDE,
                    ((y + 1U) % SIDE) * SIDE + x, ((y + SIDE - 1U) % SIDE) * SIDE + x})
                {
                    if (!visited[next] && (prepared.cells[next].water_mask & 3U) == 2U)
                    {
                        visited[next] = 1U;
                        basin.push_back(next);
                    }
                }
            }
            for (uint32_t cell : basin)
            {
                cache.basin_floor[cell] = floor;
                cache.basin_stage[cell] = stage;
            }
        }
    }
    if (cache.source)
    {
        auto const& ids = cache.atlas->configuration.model_tag == generation::TerrainModelTag::V10
            ? cache.source->v10_biome_ids : cache.source->v21_biome_ids.empty()
            ? cache.source->v9_biome_ids : cache.source->v21_biome_ids;
        if (ids.size() == COUNT) cache.biomes.assign(ids.begin(), ids.end());
        auto const& kinds = cache.atlas->configuration.model_tag == generation::TerrainModelTag::V10
            ? cache.source->v10_ice_surface_kind : cache.source->v21_ice_surface_kind.empty()
            ? cache.source->v9_ice_surface_kind : cache.source->v21_ice_surface_kind;
        if (kinds.size() == COUNT) cache.ice.assign(kinds.begin(), kinds.end());
        if (cache.atlas->configuration.model_tag == generation::TerrainModelTag::V10
            && cache.source->v10_projected_inland_ice_kind.size() == COUNT)
        {
            for (size_t i = 0U; i < COUNT; ++i)
            {
                if (cache.ice[i] == GENERATION_MAP_ICE_NONE)
                {
                    cache.ice[i] = cache.source->v10_projected_inland_ice_kind[i];
                }
            }
        }
    }
    cache.minimum_temperature = std::min_element(prepared.cells.begin(), prepared.cells.end(),
        [](auto const& a, auto const& b) { return a.ice_temperature < b.ice_temperature; })->ice_temperature - 5.0F;
    std::vector<float> temperatures;
    temperatures.reserve(prepared.cells.size());
    for (auto const& cell : prepared.cells) temperatures.push_back(cell.ice_temperature);
    auto const median = temperatures.begin() + static_cast<std::vector<float>::difference_type>(temperatures.size() / 2U);
    std::nth_element(temperatures.begin(), median, temperatures.end());
    // This transient history covers the colder half of the actual field, then
    // retreats to actual stocks. It never changes the canonical climate.
    cache.ice_age_temperature = *median + 2.5F;
    if (prepared.has_hydrology && (!cache.prepared || !cache.prepared->has_hydrology))
        cache.hydrology_elapsed = cache.last_elapsed;
    cache.wind_u.clear(); cache.wind_v.clear(); cache.current_u.clear(); cache.current_v.clear();
    cache.precipitation.clear(); cache.weather_ocean.clear();
    for (auto const& cell : prepared.cells)
    {
        cache.wind_u.push_back(cell.wind_u); cache.wind_v.push_back(cell.wind_v);
        cache.current_u.push_back(cell.current_u); cache.current_v.push_back(cell.current_v);
        cache.precipitation.push_back(cell.precipitation);
        cache.weather_ocean.push_back((cell.water_mask & 1U) != 0U ? 1U : 0U);
    }
    cache.from_cooling_contact = cache.cooling_contact;
    cache.target_cooling_contact.assign(COUNT, 0.0F);
    for (uint32_t i = 0U; i < COUNT; ++i)
    {
        int32_t const x = static_cast<int32_t>(i % SIDE), y = static_cast<int32_t>(i / SIDE);
        auto const& cell = prepared.cells[i];
        int32_t const side = static_cast<int32_t>(SIDE);
        for (auto const& [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}})
        {
            uint32_t const xx = static_cast<uint32_t>((x + dx) % side);
            uint32_t const yy = static_cast<uint32_t>((y + dy) % side);
            size_t const j = yy * SIDE + xx;
            if (cache.plates[i] == cache.plates[j])
            {
                continue;
            }
            auto const& other = prepared.cells[j];
            float const divergence = (other.plate_u - cell.plate_u) * static_cast<float>(dx)
                + (other.plate_v - cell.plate_v) * static_cast<float>(dy);
            float const strength = .92F + .08F * nativeEase(divergence);
            // Splat only the contact's heat potential, not terrain or rendered color.
            // The radial falloff reaches zero with zero slope instead of ending at a mask edge.
            constexpr int32_t RADIUS = 8;
            for (int32_t oy = -RADIUS; oy <= RADIUS; ++oy)
            {
                for (int32_t ox = -RADIUS; ox <= RADIUS; ++ox)
                {
                    float const offset_x = static_cast<float>(ox) - .5F * static_cast<float>(dx);
                    float const offset_y = static_cast<float>(oy) - .5F * static_cast<float>(dy);
                    float const distance = std::hypot(offset_x, offset_y);
                    float const contact = strength * nativeEase(1.0F - distance / RADIUS);
                    uint32_t const target_x = static_cast<uint32_t>((x + ox + side) % side);
                    uint32_t const target_y = static_cast<uint32_t>((y + oy + side) % side);
                    auto& heat = cache.target_cooling_contact[target_y * SIDE + target_x];
                    heat = std::max(heat, contact);
                }
            }
        }
    }
    if (cache.from_cooling_contact.empty())
    {
        cache.from_cooling_contact = cache.target_cooling_contact;
        cache.cooling_contact = cache.target_cooling_contact;
    }
    cache.prepared = std::move(prepared);
    cache.adoption_elapsed = cache.last_elapsed;
    if (!cache.started)
    {
        cache.started = true;
        cache.origin = cache.last_elapsed;
        cache.current_height = cache.height;
        cache.from_height = cache.height;
        cache.cooling_potential.assign(COUNT, 0.0F);
        cache.cooling_progress.assign(COUNT, 0.0F);
        cache.cooling_exposure.assign(COUNT, 0.0F);
        cache.cooling_pool.resize(COUNT);
        constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
        uint32_t pool_seed = 2'166'136'261U;
        for (double height : cache.height)
        {
            uint64_t const bits = std::bit_cast<uint64_t>(height);
            pool_seed = (pool_seed ^ static_cast<uint32_t>(bits ^ (bits >> 32U))) * 16'777'619U;
        }
        for (uint32_t y = 0U; y < SIDE; ++y) for (uint32_t x = 0U; x < SIDE; ++x)
        {
            size_t const i = y * SIDE + x;
            float pools = 0.0F;
            for (uint32_t seed = 0U; seed < 14U; ++seed)
            {
                uint32_t const hash = (seed + 17U + pool_seed) * 2'654'435'761U;
                float const px = static_cast<float>(hash % SIDE);
                float const py = static_cast<float>((hash >> 12U) % SIDE);
                float const dx = std::remainder(static_cast<float>(x) - px, static_cast<float>(SIDE));
                float const dy = std::remainder(static_cast<float>(y) - py, static_cast<float>(SIDE));
                float const radius = 7.0F + static_cast<float>((hash >> 20U) % 14U);
                if (std::abs(dx) > radius * 1.5F || std::abs(dy) > radius * 1.5F)
                {
                    continue;
                }
                // A stable periodic material warp varies the falloff, not an overlaid lake image.
                float const warp_x = (nativeRockGrain(x + px, y + py) - .5F) * radius * .65F;
                float const warp_y = (nativeRockGrain(x + py + 17.0F, y + px + 53.0F) - .5F)
                    * radius * .65F;
                float const shape_x = .85F + .30F * static_cast<float>((hash >> 4U) & 255U) / 255.0F;
                float const shape_y = .85F + .30F * static_cast<float>((hash >> 12U) & 255U) / 255.0F;
                float const distance = std::hypot((dx + warp_x) / shape_x, (dy + warp_y) / shape_y);
                pools = std::max(pools, .40F * nativeEase(1.0F - distance / radius));
            }
            cache.cooling_pool[i] = pools;
        }
    }
    if (!cell_centres && !cache.meaningful_cooling_start_seconds)
    {
        cache.meaningful_cooling_start_seconds =
            static_cast<float>((cache.last_elapsed - cache.origin).count()) / 1'000.0F;
    }
    if (!cache.image) cache.tick = std::numeric_limits<uint64_t>::max();
}

float nativeCoolingAfterglow(GenerationMapNativeCache const& cache, float seconds)
{
    if (!cache.meaningful_cooling_start_seconds)
    {
        return 0.0F;
    }
    return nativeEase((seconds - *cache.meaningful_cooling_start_seconds
        - cache.cooling_duration_seconds - .35F) / .65F);
}

void updateNativeScalarSurface(GenerationMapNativeCache& cache, float p, float filling)
{
    constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    constexpr size_t COUNT = static_cast<size_t>(SIDE) * SIDE;
    float const adoption = nativeEase(static_cast<float>((cache.last_elapsed - cache.adoption_elapsed).count()) / 900.0F);
    cache.current_height.resize(COUNT);
    for (size_t i = 0U; i < COUNT; ++i)
    {
        cache.current_height[i] = std::lerp(cache.from_height[i], cache.height[i], static_cast<double>(adoption));
    }
    auto& field = cache.fields;
    field.elevation_m.resize(COUNT); field.water_level_m.resize(COUNT); field.water_depth_m.resize(COUNT);
    field.boundary_heat.resize(COUNT); field.overlap_fraction.assign(COUNT, 0.0F); field.final_coordinates.resize(COUNT);
    // Stationary material coordinates are the safe fallback until a connected
    // moving-ownership presentation is visually validated. Field adoption remains scalar.
    for (uint32_t i = 0U; i < COUNT; ++i)
    {
        uint32_t const x = i % SIDE;
        uint32_t const y = i / SIDE;
        float const sx = static_cast<float>(x);
        float const sy = static_cast<float>(y);
        field.final_coordinates[i] = {sx, sy};
        field.elevation_m[i] = nativeSample(cache.current_height, sx, sy);
        uint32_t const si = static_cast<uint32_t>(sy) * SIDE + static_cast<uint32_t>(sx);
        double const floor = cache.basin_floor[si];
        double const stage = cache.basin_stage[si];
        field.water_level_m[i] = std::lerp(floor, stage, static_cast<double>(filling));
        field.water_depth_m[i] = filling > 0.0F && cache.prepared->has_hydrology
            && cache.prepared->cells[si].water_mask != 0U
            ? static_cast<float>(std::max(0.0, field.water_level_m[i] - field.elevation_m[i])) : 0.0F;
        bool const contact = cache.plates[si] != cache.plates[(si / SIDE) * SIDE + (si + 1U) % SIDE]
            || cache.plates[si] != cache.plates[(si + SIDE) % COUNT];
        field.boundary_heat[i] = contact ? 1.0F - nativeEase((p - .20F) / .32F) : 0.0F;
    }
}

void updateNativeCooling(GenerationMapNativeCache& cache, float seconds, std::array<float, 2U> const& range)
{
    float const adoption = nativeEase(static_cast<float>((cache.last_elapsed - cache.adoption_elapsed).count()) / 900.0F);
    float const duration = cache.cooling_duration_seconds;
    float const onset = std::min(1.5F, duration * .22F);
    float const meaningful_start = cache.meaningful_cooling_start_seconds.value_or(seconds);
    float const preview_end = std::min(seconds, meaningful_start);
    float const preview_step = std::max(0.0F, preview_end - std::max(onset, cache.cooling_elapsed_seconds));
    float const local_onset = std::max(0.0F, onset - meaningful_start);
    float const previous = std::max(local_onset, cache.cooling_elapsed_seconds - meaningful_start);
    float const current = std::max(local_onset, seconds - meaningful_start);
    float quadratic_step = 0.0F, cubic_step = 0.0F;
    if (cache.meaningful_cooling_start_seconds && current < duration && current > previous)
    {
        float const before = previous / duration, after = current / duration;
        quadratic_step = std::log((1.0F - before * before) / (1.0F - after * after));
        cubic_step = std::log((1.0F - before * before * before) / (1.0F - after * after * after));
    }
    float const span = std::max(1.0F, range[1] - range[0]);
    for (uint32_t i = 0U; i < cache.current_height.size(); ++i)
    {
        float const height_rank = std::clamp((static_cast<float>(cache.current_height[i]) - range[0]) / span, 0.0F, 1.0F);
        cache.cooling_contact[i] = std::lerp(cache.from_cooling_contact[i], cache.target_cooling_contact[i], adoption);
        float const grain = nativeRockGrain(static_cast<float>(i % 256U), static_cast<float>(i / 256U));
        float const relief_potential = std::clamp(.08F + .82F * height_rank + .12F * grain, 0.0F, 1.0F);
        uint32_t const x = i % 256U, y = i / 256U;
        double surrounding = 0.0;
        for (auto const& [dx, dy] : {std::pair{-4, 0}, std::pair{4, 0}, std::pair{0, -4}, std::pair{0, 4},
            std::pair{-3, -3}, std::pair{3, -3}, std::pair{-3, 3}, std::pair{3, 3}})
        {
            uint32_t const xx = static_cast<uint32_t>((static_cast<int32_t>(x) + dx + 256) % 256);
            uint32_t const yy = static_cast<uint32_t>((static_cast<int32_t>(y) + dy + 256) % 256);
            surrounding += cache.current_height[yy * 256U + xx] * .125;
        }
        float const depression = nativeEase(.5F
            + static_cast<float>(surrounding - cache.current_height[i]) / (span * .05F));
        // The adopted relief retains pockets in depressions without resetting their thermal age.
        float const pool_retention = (.65F + .35F * depression) * (1.0F - .65F * height_rank);
        float const pool_distance = std::clamp(1.0F - cache.cooling_pool[i] * pool_retention * 2.5F, 0.0F, 1.0F);
        float const contact_distance = 1.0F - std::clamp(cache.cooling_contact[i], 0.0F, 1.0F);
        // Slow the same relief field without clipping whole pool interiors to zero.
        float const potential = relief_potential * (.05F + .95F * pool_distance * pool_distance)
            * (.05F + .95F * contact_distance * contact_distance);
        // A rate of at least one prevents low-potential contacts from accelerating
        // into the finite endpoint; high relief still cools first.
        float const rate = 1.0F + 1.40F * (cache.cooling_potential[i] + potential) * .5F;
        cache.cooling_potential[i] = potential;
        // Provisional material cools without changing its eventual relief or thermal age.
        // H0 starts a finite budget without discarding the accumulated thermal state.
        float const preview_remaining = std::max(0.0F, .55F - cache.cooling_exposure[i]);
        cache.cooling_exposure[i] += preview_remaining * -std::expm1(-rate * preview_step / 8.0F);
        cache.cooling_exposure[i] += rate * std::lerp(cubic_step, quadratic_step, potential);
        bool const complete = cache.meaningful_cooling_start_seconds && current >= duration;
        float const progress = complete ? 1.0F : -std::expm1(-cache.cooling_exposure[i]);
        cache.cooling_progress[i] = std::max(cache.cooling_progress[i], progress);
    }
    cache.cooling_elapsed_seconds = seconds;
}

// Rasterize the same vector primitives the interactive map submits to the GPU.
void rasterNativeWeather(GenerationMapImage& image, std::span<GuiVectorVertex const> triangles)
{
    auto edge=[](auto const& a,auto const& b,float x,float y) {
        return (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]);
    };
    for (size_t i=0U;i+2U<triangles.size();i+=3U)
    {
        auto const& a=triangles[i]; auto const& b=triangles[i+1U]; auto const& c=triangles[i+2U];
        float const area=edge(a.position,b.position,c.position[0U],c.position[1U]);
        if (std::abs(area)<.00001F) continue;
        int32_t const left=std::max(0,static_cast<int32_t>(std::floor(std::min({a.position[0],b.position[0],c.position[0]}))));
        int32_t const top=std::max(0,static_cast<int32_t>(std::floor(std::min({a.position[1],b.position[1],c.position[1]}))));
        int32_t const right=std::min(static_cast<int32_t>(image.width),
            static_cast<int32_t>(std::ceil(std::max({a.position[0],b.position[0],c.position[0]}))));
        int32_t const bottom=std::min(static_cast<int32_t>(image.height),
            static_cast<int32_t>(std::ceil(std::max({a.position[1],b.position[1],c.position[1]}))));
        for (int32_t y=top;y<bottom;++y) for (int32_t x=left;x<right;++x)
        {
            float alpha=0.0F;
            for (uint32_t sample=0U;sample<4U;++sample)
            {
                float const px=x+.25F+.5F*(sample%2U),py=y+.25F+.5F*(sample/2U);
                float const wa=edge(b.position,c.position,px,py)/area;
                float const wb=edge(c.position,a.position,px,py)/area;
                float const wc=1.0F-wa-wb;
                if (wa>=0.0F && wb>=0.0F && wc>=0.0F)
                    alpha+=(wa*a.rgba[3]+wb*b.rgba[3]+wc*c.rgba[3])/(4.0F*255.0F);
            }
            if (alpha>0.0F) blendPixel(image.rgba[static_cast<size_t>(y)*image.width+x],a.rgba,alpha);
        }
    }
}

std::shared_ptr<GenerationMapImage> renderNativeSurface(GenerationMapNativeCache& cache,float p,float seconds)
{
    constexpr uint32_t SIDE=GENERATION_MAP_ANIMATION_RESOLUTION, SCALE=GENERATION_MAP_ANIMATION_SCALE;
    auto const& prepared=*cache.prepared;auto& field=cache.fields;
    float const filling = nativeEase((p - .30F) / .28F) * nativeCoolingAfterglow(cache, seconds)
        * nativeEase(static_cast<float>((cache.last_elapsed - cache.hydrology_elapsed).count())
            / (2'000.0F * SLOW_STAGE_PACING));
    updateNativeScalarSurface(cache, p, filling);
    auto bounds=std::minmax_element(cache.current_height.begin(),cache.current_height.end());
    std::array<float,2U> range{static_cast<float>(*bounds.first),static_cast<float>(*bounds.second)};
    float const span=std::max(1.0F,range[1]-range[0]);
    updateNativeCooling(cache, seconds, range);
    // Collision uplift belongs to this same scalar surface and vanishes before settling.
    float const collision=nativeEase((p-.20F)/.10F)*(1.0F-nativeEase((p-.48F)/.10F));
    for(size_t i=0U;i<field.elevation_m.size();++i)field.elevation_m[i]+=field.overlap_fraction[i]*span*.035F*collision;
    auto image=std::make_shared<GenerationMapImage>();
    image->width=image->height=SIDE*SCALE;
    image->rgba.resize(image->width*image->height);
    MapImageDraw river_draw;
    river_draw.destination = {0.0F, 0.0F, static_cast<float>(image->width), static_cast<float>(image->height)};
    auto const river_layer = cache.source ? rasterizeS2RiverMap(cache.source->v10_river_geometry,
        std::span{&river_draw, 1U}, image->width, image->height, image->width, image->height) : nullptr;
    std::vector<uint8_t> river_frozen(river_layer ? static_cast<size_t>(SIDE) * SIDE : 0U);
    std::vector<std::array<float, 3U>> river_ice_color(river_frozen.size());
    std::vector<float> energy(static_cast<size_t>(SIDE)*SIDE);
    float const green=nativeEase((p-.49F)/.09F);
    // Peak hold is .08 of the story, reduced from .12; the same front then retreats.
    float const ice_growth=nativeEase((p-.58F)/.10F);
    float const age=ice_growth*(1.0F-nativeEase((p-.76F)/.14F));
    float const land_weathering=nativeEase((p-.47F)/.11F),dust_growth=nativeEase((p-.35F)/.12F);
    for(uint32_t y=0U;y<SIDE;++y)for(uint32_t x=0U;x<SIDE;++x)
    {
        uint32_t const i=y*SIDE+x;auto uv=field.final_coordinates[i];
        uint32_t sx=static_cast<uint32_t>(uv[0])%SIDE,sy=static_cast<uint32_t>(uv[1])%SIDE,si=sy*SIDE+sx;
        auto const& cell=prepared.cells[si];bool final_wet=cell.water_mask!=0U;
        float h=static_cast<float>(field.elevation_m[i]);
        bool const river_only = prepared.has_hydrology && (cell.water_mask & 7U) == 4U;
        float const depth = river_only ? 0.0F : field.water_depth_m[i];
        float const wet_cover = river_only ? (river_layer ? 0.0F : filling) : depth > 0.0F ? 1.0F : 0.0F;
        uint32_t east=y*SIDE+(x+1U)%SIDE,south=((y+1U)%SIDE)*SIDE+x;
        std::array<float,2U> neighbor{static_cast<float>(field.elevation_m[east]),static_cast<float>(field.elevation_m[south])};
        auto dry=s2NativeTerrainColor(h,range,neighbor,false,0.0F,0U);
        auto wet=s2NativeTerrainColor(h,range,neighbor,true,depth,0U);
        if(h>0.0F)
        {
            // The elevation palette can make inland water look like vegetation.
            // During formation show its actual wet footprint as blue water;
            // canonical elevation coloring returns only with the final climate.
            constexpr std::array<float,3U> FORMING_WATER{45.0F,87.0F,107.0F};
            for(uint32_t c=0U;c<3U;++c) wet[c]=std::lerp(FORMING_WATER[c],wet[c],land_weathering);
        }
        std::array<float,3U> base{};for(uint32_t c=0U;c<3U;++c)base[c]=std::lerp(dry[c],wet[c],wet_cover);
        float const desert = green * age;
        // The same climate field becomes drier as the age locks humidity in ice.
        float const aridity = nativeEase((.65F + .65F * age - cell.precipitation) / .45F);
        uint8_t kind=cache.ice[si];
        if (wet_cover < .5F)
        {
            // Before the ice advance, vegetation also occupies future frozen land.
            uint8_t const biome = kind == 0U && cache.biomes[si] != 255U ? cache.biomes[si] : 2U;
            if (!(river_layer && river_only))
            {
                base=s2NativeBiomeColor(base,biome,green);
            }
        }
        if (cell.temperature > 0.0F && wet_cover < .5F)
        {
            base = s2NativeBiomeColor(base, 5U, desert * aridity);
        }
        // One spatial ice amount owns the complete material. Modern stock is its
        // persistent floor after growth; the past age only extends the same front.
        float const temperature_span=std::max(1.0F,cache.ice_age_temperature-cache.minimum_temperature);
        float const stock_rank=.05F+.90F*nativeEase(
            (cell.ice_temperature-cache.minimum_temperature)/temperature_span);
        float const stock_amount=kind!=GENERATION_MAP_ICE_NONE && ice_growth>0.0F
            ? ice_growth-stock_rank : -1.0F;
        float const threshold=std::lerp(cache.minimum_temperature,cache.ice_age_temperature,age);
        float const age_amount=age>0.0F ? (threshold-cell.ice_temperature)/5.0F : -1.0F;
        float const ice_amount=std::max(stock_amount,age_amount);
        bool const frozen=ice_amount>=0.0F;
        uint8_t const ice_kind=kind!=GENERATION_MAP_ICE_NONE
            ? (final_wet && kind==GENERATION_MAP_ICE_WATER ? 3U : kind)
            : (final_wet ? 3U : 2U);
        auto const ice_material=s2NativeTerrainColor(h,range,neighbor,final_wet,depth,ice_kind);
        if (river_layer)
        {
            river_frozen[i] = static_cast<uint8_t>(frozen);
            river_ice_color[i] = ice_material;
        }
        auto const material = nativeLavaMaterial(uv[0], uv[1], cache.cooling_progress[si], seconds, si);
        float const liquid = material.exposed_melt * (1.0F - wet_cover);
        float slope=(neighbor[0]-h)+.65F*(neighbor[1]-h);
        float shade=std::clamp(.82F-slope/span*4.0F,.55F,1.12F);
        GenerationMapRgba8 color{0U,0U,0U,255U};
        energy[i] = 0.13F * liquid;
        for(uint32_t c=0U;c<3U;++c)
        {
            // Basalt weathers into the already living pre-glacial landscape.
            // The same biome field returns as the ice front retreats.
            constexpr std::array<float,3U> DUST{122.0F,113.0F,94.0F};
            auto const desert_dust=s2NativeBiomeColor(DUST,5U,desert);
            float const basalt = std::lerp(material.crust[c],
                static_cast<float>(COOLING_WATER_CRUST_COLOR[c]), material.crust_weathering);
            float const crust = std::lerp(basalt, desert_dust[c], dust_growth) * shade;
            float const weathering=wet_cover > .5F ? 1.0F : land_weathering;
            float rock=std::lerp(crust,base[c],weathering);
            if(frozen && !(river_layer && river_only))
            {
                rock=ice_material[c];
            }
            float const value = std::lerp(rock, material.molten[c], liquid);
            color[c]=static_cast<uint8_t>(std::clamp(std::lround(value),0L,255L));
        }
        for(uint32_t oy=0U;oy<SCALE;++oy)for(uint32_t ox=0U;ox<SCALE;++ox)
            image->rgba[(y*SCALE+oy)*image->width+x*SCALE+ox]=color;
    }
    nativeThermalLight(*image,energy);
    if (river_layer)
    {
        // Retain this timeline's existing river filling and temperature/stock
        // freezing decisions; only the footprint comes from the shared curves.
        // Standing coverage is excluded by the shared primitive's owner mask.
        for (uint32_t y = 0U; y < image->height; ++y)
        {
            for (uint32_t x = 0U; x < image->width; ++x)
            {
                size_t const index = static_cast<size_t>(y) * image->width + x;
                size_t const cell = static_cast<size_t>(y / SCALE) * SIDE + x / SCALE;
                float const coverage = river_layer->v10_river_coverage[index] * filling;
                if (!(coverage > 0.0F))
                {
                    continue;
                }
                constexpr std::array<float, 3U> WATER{45.0F, 87.0F, 107.0F};
                auto const& color = river_frozen[cell] != 0U ? river_ice_color[cell] : WATER;
                for (size_t c = 0U; c < 3U; ++c)
                {
                    image->rgba[index][c] = static_cast<uint8_t>(std::clamp(std::lround(std::lerp(
                        static_cast<float>(image->rgba[index][c]), color[c], coverage)), 0L, 255L));
                }
            }
        }
    }
    return image;
}
} // namespace

std::optional<GenerationMapThermalSample> GenerationMapAnimator::thermalSample(uint32_t x, uint32_t y) const noexcept
{
    constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    if (!native_cache_ || x >= SIDE || y >= SIDE)
    {
        return std::nullopt;
    }
    auto const& cache = *native_cache_;
    size_t const i = y * SIDE + x;
    if (i >= cache.cooling_progress.size())
    {
        return std::nullopt;
    }
    return GenerationMapThermalSample{cache.cooling_potential[i], cache.cooling_progress[i],
        static_cast<float>(cache.current_height[i])};
}

float generationMapMaterialGrain(float x, float y) noexcept
{
    return nativeRockGrain(x,y);
}

GenerationMapAnimationFrame GenerationMapAnimator::updateNative(
    std::shared_ptr<generation::GenerationSnapshot const> snapshot,
    std::shared_ptr<generation::GlobalAtlas const> atlas, std::shared_ptr<S2MapImage const> source,
    GenerationMapAnimationClock const& clock)
{
    if (!native_cache_) native_cache_ = std::make_unique<GenerationMapNativeCache>();
    auto& cache = *native_cache_;
    cache.cooling_duration_seconds = std::max(.001F, static_cast<float>(clock.cooling_duration.count()) / 1'000.0F);
    bool const previously_started = cache.started;
    auto const previous_elapsed = cache.last_elapsed;
    cache.last_elapsed = std::max(cache.last_elapsed, clock.visible_elapsed);
    if (!cache.source && snapshot && snapshot != cache.snapshot)
    {
        auto prepared = prepareAnimationMap(*snapshot, {});
        if (prepared && (snapshot->v10 || snapshot->v21 || snapshot->v9))
        {
            std::vector<double> heights;
            if (snapshot->v10)
            {
                auto const& fields = *snapshot->v10;
                heights = fields.source_elevation_m;
                if (heights.empty()) heights.assign(fields.height_raw.begin(), fields.height_raw.end());
            }
            else
            {
                auto const& values = snapshot->v21 ? snapshot->v21->source_elevation_m : snapshot->v9->source_elevation_m;
                heights.assign(values.begin(), values.end());
            }
            auto const& plates = snapshot->v10 ? snapshot->v10->plate_ids
                : snapshot->v21 ? snapshot->v21->plate_ids : snapshot->v9->plate_ids;
            if (heights.size() == 256U * 256U)
            {
                cache.snapshot = snapshot;
                adoptNativeStory(cache, std::move(*prepared), heights, plates,
                    snapshot->stage == generation::GenerationStage::V21Platec
                        || snapshot->stage == generation::GenerationStage::V9Platec
                        || snapshot->stage == generation::GenerationStage::V10Platec);
            }
        }
    }
    if (atlas && source && source != cache.source
        && source->source_identity.terrain_version == generation::terrainVersion(atlas->configuration.model_tag)
        && source->source_identity.seed == atlas->configuration.seed
        && source->source_identity.configuration_identity == generation::configurationIdentity(atlas->configuration))
    {
        auto prepared = prepareAnimationMap(*atlas, {});
        auto const& heights = atlas->configuration.model_tag == generation::TerrainModelTag::V10
            ? source->v10_elevation_m : atlas->configuration.model_tag == generation::TerrainModelTag::V21
                ? source->v21_elevation_m : source->v9_elevation_m;
        auto const& plates = atlas->configuration.model_tag == generation::TerrainModelTag::V10
            ? source->v10_plate_ids : atlas->configuration.model_tag == generation::TerrainModelTag::V21
                ? source->v21_plate_ids : source->v9_plate_ids;
        if (prepared && heights.size() == 256U * 256U && source->width == 512U && source->height == 512U)
        {
            cache.atlas = std::move(atlas); cache.source = std::move(source);
            adoptNativeStory(cache, std::move(*prepared), heights, plates, false);
        }
    }
    // The approved screenshot remains visible only until the first real source field.
    if (!cache.prepared) return {};
    float const seconds = static_cast<float>((cache.last_elapsed - cache.origin).count()) / 1'000.0F;
    float const dt = previously_started
        ? static_cast<float>((cache.last_elapsed - previous_elapsed).count()) / 28'000.0F : 0.0F;
    float const source_cap = !cache.prepared->has_climate ? .30F
        : !cache.prepared->has_hydrology ? .57F : !cache.source ? .97F : 1.0F;
    float const cap = source_cap;
    if (clock.reduced_motion && cache.source) cache.progress = 1.0F;
    else cache.progress = advanceNativeProgress(cache.progress, cap, dt);
    if (!clock.reduced_motion && nativeCoolingAfterglow(cache, seconds) <= 0.0F)
    {
        cache.progress = std::min(cache.progress, std::nextafter(.30F, 0.0F));
    }
    float const p = cache.progress;
    uint64_t const tick = static_cast<uint64_t>(cache.last_elapsed.count()) * 24U / 1'000U;
    if (!cache.image || tick != cache.tick || clock.reduced_motion != cache.reduced_motion)
    {
        cache.reduced_motion = clock.reduced_motion;
        if (p >= 1.0F && cache.source)
        {
            if (std::any_of(cache.cooling_progress.begin(), cache.cooling_progress.end(),
                [](float progress) { return progress < 1.0F; }) || cache.current_height != cache.height)
            {
                // Canonical pixels are fully solid even when history was skipped.
                // Inspection must describe that same final geometry and thermal state.
                cache.current_height = cache.height;
                cache.from_cooling_contact = cache.target_cooling_contact;
                auto const bounds = std::minmax_element(cache.current_height.begin(), cache.current_height.end());
                updateNativeCooling(cache, seconds,
                    {static_cast<float>(*bounds.first), static_cast<float>(*bounds.second)});
                std::fill(cache.cooling_progress.begin(), cache.cooling_progress.end(), 1.0F);
            }
            cache.surface = std::make_shared<GenerationMapImage const>(GenerationMapImage{
                cache.source->width, cache.source->height, cache.source->rgba});
        }
        else cache.surface = renderNativeSurface(cache, p, seconds);
        auto decorated = std::make_shared<GenerationMapImage>(*cache.surface);
        float const weather = nativeEase((p - .90F) / .10F);
        float const rain = nativeCoolingAfterglow(cache, seconds)
            * nativeEase((p - .30F) / .06F) * (1.0F - nativeEase((p - .54F) / .06F));
        S2MapWeatherFields const weather_fields{
            .resolution=GENERATION_MAP_ANIMATION_RESOLUTION,
            .ocean_resolution=GENERATION_MAP_ANIMATION_RESOLUTION,
            .precipitation_resolution=GENERATION_MAP_ANIMATION_RESOLUTION,
            .wind_u=cache.wind_u, .wind_v=cache.wind_v,
            .current_u=cache.current_u, .current_v=cache.current_v,
            .precipitation=cache.precipitation, .ocean=cache.weather_ocean,
            .precipitation_maximum=1.0F,
        };
        S2MapLayout layout;
        layout.world_draw_count=1U;
        layout.world_draws[0U].destination={0.0F,0.0F,
            static_cast<float>(decorated->width),static_cast<float>(decorated->height)};
        layout.world_draws[0U].scissor={0U,0U,decorated->width,decorated->height};
        std::vector<GuiVectorVertex> weather_triangles;
        if (weather > 0.0F)
            appendS2MapWeatherTriangles(weather_triangles,layout,weather_fields,seconds,1.0F,
                cache.prepared->has_wind,cache.prepared->has_currents,false,weather);
        if (rain > 0.0F && cache.prepared->has_precipitation)
            appendS2MapWeatherTriangles(weather_triangles,layout,weather_fields,seconds,1.0F,
                false,false,true,rain);
        rasterNativeWeather(*decorated,weather_triangles);
        cache.image = std::move(decorated); cache.tick = tick;
    }
    GenerationMapAnimationPhase phase; float begin, end;
    if (p < .14F) {phase=GenerationMapAnimationPhase::Creation;begin=0;end=.14F;}
    else if (p < .30F) {phase=GenerationMapAnimationPhase::Tectonics;begin=.14F;end=.30F;}
    else if (p < .58F) {phase=GenerationMapAnimationPhase::Water;begin=.30F;end=.58F;}
    else if (p < .90F) {phase=GenerationMapAnimationPhase::Ice;begin=.58F;end=.90F;}
    else if (p < 1.0F) {phase=GenerationMapAnimationPhase::Climate;begin=.90F;end=1;}
    else {phase=GenerationMapAnimationPhase::Arrival;begin=0;end=1;}
    return {phase,(p-begin)/(end-begin),cache.image,cache.surface};
}

GenerationMapAnimationFrame GenerationMapAnimator::updateNative(
    std::shared_ptr<generation::GlobalAtlas const> atlas, std::shared_ptr<S2MapImage const> source,
    GenerationMapAnimationClock const& clock)
{
    return updateNative(nullptr, std::move(atlas), std::move(source), clock);
}

GenerationMapAnimator::GenerationMapAnimator() = default;
GenerationMapAnimator::~GenerationMapAnimator() = default;
GenerationMapAnimator::GenerationMapAnimator(GenerationMapAnimator&&) noexcept = default;
GenerationMapAnimator& GenerationMapAnimator::operator=(GenerationMapAnimator&&) noexcept = default;

void GenerationMapAnimator::reset() noexcept
{
    cache_.reset();
    native_cache_.reset();
}

GenerationMapAnimationFrame GenerationMapAnimator::update(
    std::shared_ptr<generation::GenerationSnapshot const> snapshot,
    std::shared_ptr<generation::GlobalAtlas const> matching_atlas,
    GenerationMapImageHandle final_image,
    GenerationMapDisplayIceMaskHandle final_display_ice_mask,
    GenerationMapAnimationClock const& clock
)
{
    auto const visible_elapsed = std::max(clock.visible_elapsed, std::chrono::milliseconds::zero());
    if (cache_ == nullptr || visible_elapsed < cache_->last_visible_elapsed)
    {
        cache_ = std::make_unique<GenerationMapAnimationCache>();
    }
    auto const elapsed_delta = visible_elapsed - cache_->last_visible_elapsed;
    cache_->last_visible_elapsed = visible_elapsed;
    auto presentationLimit = [&](PreparedAnimationMap const& data) {
        int64_t const duration = presentationDuration(clock.formation_duration).count();
        if (!data.has_hydrology || !data.has_precipitation)
        {
            return std::chrono::milliseconds{duration * 20 / 100};
        }
        if (!data.ice_fields_available)
        {
            return std::chrono::milliseconds{duration * 60 / 100};
        }
        if (!data.has_climate || (data.weather_fields_required && (!data.has_wind || !data.has_currents)))
        {
            return std::chrono::milliseconds{duration * 90 / 100};
        }
        return std::chrono::milliseconds::max();
    };
    bool const had_prepared = cache_->prepared.has_value();
    if (had_prepared && cache_->prepared->native_palette)
    {
        // Time spent waiting for authoritative fields cannot consume their visible phase.
        auto const limit = presentationLimit(*cache_->prepared);
        if (!cache_->prepared->has_hydrology || !cache_->prepared->has_precipitation)
        {
            // Continue cooling while source work runs, retaining molten regions
            // until water can form instead of holding a fully cooled blank map.
            double const cooling_limit = static_cast<double>(limit.count()) * 0.7;
            double const remaining = std::max(0.0, cooling_limit
                - static_cast<double>(cache_->presentation_elapsed.count()));
            auto const cooling_delta = std::chrono::milliseconds{static_cast<int64_t>(std::lround(
                cooling_limit > 0.0
                    ? remaining * -std::expm1(-static_cast<double>(elapsed_delta.count()) / cooling_limit)
                    : 0.0))};
            cache_->presentation_elapsed += cooling_delta;
        }
        else
        {
            cache_->presentation_elapsed = std::max(cache_->presentation_elapsed,
                std::min(cache_->presentation_elapsed + elapsed_delta, limit));
        }
    }
    bool source_changed = false;
    AnimationPhaseState phase = animationPhase(clock);
    GenerationMapAnimationFrame frame{
        .phase = phase.phase,
        .phase_progress = phase.progress,
    };
    GenerationMapUpdateTiming const timing{*cache_, frame};
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
        // Native V9 atlases have no legacy fields; retain their valid snapshot preparation too.
        bool const cache_matches = cache_ != nullptr
            && cache_->atlas == matching_atlas && cache_->display_ice_mask == final_display_ice_mask;
        if (!cache_matches)
        {
            std::optional<PreparedAnimationMap> prepared = prepareAnimationMap(*matching_atlas, final_display_ice_mask);
            if (prepared)
            {
                cache_->snapshot.reset();
                cache_->atlas = matching_atlas;
                cache_->display_ice_mask = final_display_ice_mask;
                cache_->prepared = std::move(prepared);
                cache_->uses_atlas = true;
                source_changed = true;
            }
            else
            {
                cache_->atlas = matching_atlas;
                cache_->display_ice_mask = final_display_ice_mask;
                // Keep the last valid snapshot until a usable replacement arrives.
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
            if (prepared)
            {
                cache_->snapshot = snapshot;
                cache_->atlas = matching_atlas;
                cache_->display_ice_mask = final_display_ice_mask;
                cache_->prepared = std::move(prepared);
                cache_->uses_atlas = false;
                source_changed = true;
            }
        }
    }

    if (cache_->prepared && cache_->prepared->native_palette)
    {
        auto const limit = presentationLimit(*cache_->prepared);
        if (!had_prepared)
        {
            cache_->presentation_elapsed = std::min(visible_elapsed, limit);
        }
        auto presentation_clock = clock;
        presentation_clock.visible_elapsed = cache_->presentation_elapsed;
        if (cache_->presentation_elapsed == limit)
        {
            presentation_clock.visible_elapsed = limit - std::chrono::milliseconds{1};
        }
        phase = animationPhase(presentation_clock, true);
        frame.phase = phase.phase_index == 0U ? GenerationMapAnimationPhase::Creation : phase.phase;
        frame.phase_progress = phase.progress;
    }
    if (source_changed)
    {
        cache_->source_transition_image = cache_->rendered_image;
        cache_->source_transition_started = visible_elapsed;
        cache_->rendered_image.reset();
    }
    if (cache_ == nullptr || !cache_->prepared)
    {
        if (phase.phase == GenerationMapAnimationPhase::Arrival && final_image != nullptr)
        {
            frame.image = std::move(final_image);
        }
        return frame;
    }
    if (phase.phase == GenerationMapAnimationPhase::Arrival && final_image != nullptr)
    {
        PreparedAnimationMap const& data = *cache_->prepared;
        if (!data.has_wind && !data.has_currents)
        {
            frame.image = std::move(final_image);
            return frame;
        }
        if (final_image->width == 0U || final_image->height != final_image->width
            || final_image->rgba.size() != static_cast<size_t>(final_image->width) * final_image->height)
        {
            frame.image = std::move(final_image);
            return frame;
        }
        auto animated = std::make_shared<GenerationMapImage>(*final_image);
        auto const presentation_duration = presentationDuration(clock.formation_duration);
        float const flow_progress = 1.0F + static_cast<float>(
            std::max<int64_t>((visible_elapsed - presentation_duration).count(), 0)
        ) / 1'000.0F;
        animateWeather(*animated, data, flow_progress, true, true, false);
        frame.image = std::move(animated);
        return frame;
    }
    uint32_t const visual_phase_index = std::min(phase.phase_index, 4U);
    if (cache_->final_image != final_image)
    {
        GenerationMapImageHandle const previous_image = cache_->rendered_image;
        bool const same_phase = previous_image != nullptr && cache_->rendered_phase_index == phase.phase_index;
        cache_->final_image = final_image;
        cache_->rendered_image.reset();
        cache_->transition_image = previous_image;
        cache_->transition_phase_index = previous_image != nullptr && phase.phase_index < 5U
            ? std::optional<uint32_t>{phase.phase_index} : std::nullopt;
        cache_->transition_start_progress = same_phase ? phase.progress : 0.0F;
        cache_->transition_duration = same_phase
            ? std::max(0.01F, std::min(0.1F, 1.0F - phase.progress)) : 0.1F;
    }
    if (cache_->transition_phase_index != phase.phase_index)
    {
        cache_->transition_image.reset();
        cache_->transition_phase_index = phase.phase_index;
        cache_->transition_start_progress = 0.0F;
        cache_->transition_duration = 0.1F;
        if (phase.phase_index > 0U && phase.phase_index < 5U)
        {
            bool const cached_frame_is_previous_phase = cache_->rendered_image != nullptr
                && cache_->rendered_phase_index + 1U == phase.phase_index;
            cache_->transition_image = cached_frame_is_previous_phase ? cache_->rendered_image : nullptr;
            if (cache_->transition_image == nullptr)
            {
                AnimationPhaseState const previous_phase{
                    .phase = animationPhaseForIndex(phase.phase_index - 1U),
                    .phase_index = phase.phase_index - 1U,
                    .progress = 1.0F,
                };
                GenerationMapImage previous_image = renderAnimationFrame(
                    *cache_->prepared, previous_phase, clock.reduced_motion, final_image.get()
                );
                cache_->transition_image = std::make_shared<GenerationMapImage const>(std::move(previous_image));
            }
        }
    }
    auto const& phase_weights = cache_->prepared->native_palette
        ? NATIVE_ANIMATION_PHASE_WEIGHTS : ANIMATION_PHASE_WEIGHTS;
    uint32_t const phase_duration = static_cast<uint32_t>(presentationDuration(clock.formation_duration).count()
        * phase_weights[visual_phase_index] / 100);
    uint32_t const visual_ticks = std::max(1U, phase_duration * 24U / 1'000U);
    uint32_t const tick = static_cast<uint32_t>(std::lround(phase.progress * visual_ticks));
    uint32_t const image_tick = phase.phase_index == 0U && cache_->prepared->native_palette
        ? static_cast<uint32_t>(std::lround(static_cast<double>(visible_elapsed.count()) * 24.0 / 1'000.0)) : tick;
    uint32_t const source_transition_tick = static_cast<uint32_t>(
        (visible_elapsed - cache_->source_transition_started).count() * 24 / 1'000);
    if (!cache_->rendered_image || cache_->rendered_phase_index != phase.phase_index || cache_->rendered_tick != image_tick
        || (cache_->source_transition_image != nullptr
            && cache_->rendered_source_transition_tick != source_transition_tick))
    {
        AnimationPhaseState render_phase = phase;
        render_phase.progress = static_cast<float>(tick) / static_cast<float>(visual_ticks);
        GenerationMapImage rendered = renderAnimationFrame(
            *cache_->prepared, render_phase, clock.reduced_motion, final_image.get(),
            static_cast<float>(visible_elapsed.count()) / 1'000.0F
        );
        if (cache_->transition_image != nullptr && cache_->transition_phase_index == phase.phase_index)
        {
            float const transition_progress = smoothProgress(std::clamp(
                (render_phase.progress - cache_->transition_start_progress) / cache_->transition_duration,
                0.0F,
                1.0F
            ));
            for (size_t index = 0U; index < rendered.rgba.size(); ++index)
            {
                rendered.rgba[index] = blendColor(
                    cache_->transition_image->rgba[index], rendered.rgba[index], transition_progress
                );
            }
            if (render_phase.progress >= cache_->transition_start_progress + cache_->transition_duration)
            {
                cache_->transition_image.reset();
            }
        }
        if (cache_->source_transition_image != nullptr)
        {
            float const blend = smoothProgress(std::clamp(
                static_cast<float>((visible_elapsed - cache_->source_transition_started).count()) / 300.0F,
                0.0F, 1.0F));
            if (cache_->source_transition_image->rgba.size() == rendered.rgba.size())
            {
                for (size_t index = 0U; index < rendered.rgba.size(); ++index)
                {
                    rendered.rgba[index] = blendColor(
                        cache_->source_transition_image->rgba[index], rendered.rgba[index], blend);
                }
            }
            if (blend >= 1.0F)
            {
                cache_->source_transition_image.reset();
            }
        }
        cache_->rendered_image = std::make_shared<GenerationMapImage const>(std::move(rendered));
        cache_->rendered_phase_index = phase.phase_index;
        cache_->rendered_tick = image_tick;
        cache_->rendered_source_transition_tick = source_transition_tick;
    }
    frame.image = cache_->rendered_image;
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
    if ((clock.ready || phase.phase == GenerationMapAnimationPhase::Arrival) && final_image != nullptr) {
        if (clock.reduced_motion) {
            return final_image;
        }
        std::optional<PreparedAnimationMap> const prepared = prepareAnimationMap(snapshot, final_display_ice_mask);
        if (!prepared || (!prepared->has_wind && !prepared->has_currents)
            || final_image->width == 0U || final_image->height != final_image->width
            || final_image->rgba.size() != static_cast<size_t>(final_image->width) * final_image->height)
        {
            return final_image;
        }
        auto animated = std::make_shared<GenerationMapImage>(*final_image);
        auto const presentation_duration = presentationDuration(clock.formation_duration);
        int64_t const elapsed_after_formation = std::max<int64_t>(
            (std::max(clock.visible_elapsed, std::chrono::milliseconds::zero()) - presentation_duration).count(),
            0
        );
        float const flow_progress = 1.0F + static_cast<float>(elapsed_after_formation) / 1'000.0F;
        animateWeather(*animated, *prepared, flow_progress, true, true, false);
        return animated;
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
        renderAnimationFrame(*prepared, phase, clock.reduced_motion, final_image.get())
    );
}

} // namespace client
