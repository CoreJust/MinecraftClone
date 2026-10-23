#include <client/GenerationMapPresentation.hpp>

#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace client
{
namespace
{

static constexpr uint32_t MINIMUM_RESOLUTION = 256U;
static constexpr uint32_t MAXIMUM_RESOLUTION = 512U;

struct MapColorStop final
{
    float value = 0.0F;
    GenerationMapRgba8 color{};
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
bool hasValidResolution(generation::GenerationSnapshot const& snapshot) noexcept
{
    return snapshot.resolution == MINIMUM_RESOLUTION
        || snapshot.resolution == MAXIMUM_RESOLUTION;
}

[[nodiscard]]
bool hasExactLength(std::vector<float> const& field, uint32_t const expected_count) noexcept
{
    return field.size() == static_cast<decltype(field.size())>(expected_count);
}

[[nodiscard]]
bool hasExactLength(std::vector<uint8_t> const& field, uint32_t const expected_count) noexcept
{
    return field.size() == static_cast<decltype(field.size())>(expected_count);
}

[[nodiscard]]
bool isFiniteField(std::vector<float> const& field, uint32_t const expected_count) noexcept
{
    if (!hasExactLength(field, expected_count))
    {
        return false;
    }

    for (float const value : field)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool isMaskField(std::vector<uint8_t> const& field, uint32_t const expected_count) noexcept
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
bool hasValidElevation(generation::GenerationSnapshot const& snapshot, uint32_t const count) noexcept
{
    return isFiniteField(snapshot.elevation, count);
}

[[nodiscard]]
bool hasValidHydrology(generation::GenerationSnapshot const& snapshot, uint32_t const count) noexcept
{
    return isFiniteField(snapshot.water_level, count)
        && isMaskField(snapshot.ocean_mask, count)
        && isMaskField(snapshot.lake_mask, count)
        && isMaskField(snapshot.river_mask, count);
}

[[nodiscard]]
bool hasValidClimate(generation::GenerationSnapshot const& snapshot, uint32_t const count) noexcept
{
    return isFiniteField(snapshot.temperature, count)
        && isFiniteField(snapshot.precipitation, count);
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
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    float const water_level,
    uint8_t const ocean,
    uint8_t const lake,
    uint8_t const river
) noexcept
{
    if (ocean != 0U)
    {
        float const depth = std::clamp(water_level - elevation, 0.0F, 2.0F);
        float const strength = 0.16F + depth * 0.07F;
        return blendColor(terrain_color, { 44U, 97U, 121U, 255U }, strength);
    }
    if (lake != 0U)
    {
        return blendColor(terrain_color, { 76U, 143U, 151U, 255U }, 0.42F);
    }
    if (river != 0U)
    {
        return blendColor(terrain_color, { 83U, 142U, 145U, 255U }, 0.42F);
    }
    return terrain_color;
}

[[nodiscard]]
GenerationMapRgba8 climateColor(float const temperature, float const precipitation) noexcept
{
    GenerationMapRgba8 const temperature_tint = rampColor(temperature, TEMPERATURE_COLORS);
    GenerationMapRgba8 const precipitation_tint = rampColor(precipitation, PRECIPITATION_COLORS);
    return blendColor(temperature_tint, precipitation_tint, 0.5F);
}

[[nodiscard]]
GenerationMapRgba8 frozenSurfaceColor(
    GenerationMapRgba8 const& color,
    float const temperature,
    bool const ocean
) noexcept
{
    // Land follows canonical cold-solid material; ocean tint stays strictly below freezing.
    static constexpr float FREEZING_TEMPERATURE_C = -2.0F;
    if (ocean)
    {
        if (temperature < FREEZING_TEMPERATURE_C)
        {
            return blendColor(color, { 184U, 221U, 232U, 255U }, 0.78F);
        }
        return color;
    }
    if (temperature <= FREEZING_TEMPERATURE_C)
    {
        return blendColor(color, { 245U, 248U, 247U, 255U }, 0.76F);
    }
    return color;
}

[[nodiscard]]
GenerationMapRgba8 shadedColor(
    GenerationMapRgba8 const& color,
    std::vector<float> const& elevation,
    uint32_t const resolution,
    uint32_t const index
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
    float const east_west_slope = elevation[east_index] - elevation[west_index];
    float const south_north_slope = elevation[south_index] - elevation[north_index];
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

} // namespace

std::optional<GenerationMapImage> rasterizeGenerationMap(
    generation::GenerationSnapshot const& snapshot
)
{
    if (!hasValidResolution(snapshot))
    {
        return std::nullopt;
    }

    uint32_t const pixel_count = snapshot.resolution * snapshot.resolution;
    if (!hasValidElevation(snapshot, pixel_count))
    {
        return std::nullopt;
    }

    bool uses_hydrology = false;
    bool uses_climate = false;
    switch (snapshot.stage)
    {
    case generation::GenerationStage::Tectonics:
    case generation::GenerationStage::CoastalElaboration:
        break;
    case generation::GenerationStage::InitialDrainage:
        if (!hasValidHydrology(snapshot, pixel_count))
        {
            return std::nullopt;
        }
        uses_hydrology = true;
        break;
    case generation::GenerationStage::ProvisionalClimate:
    case generation::GenerationStage::FinalClimate:
        if (!hasValidClimate(snapshot, pixel_count))
        {
            return std::nullopt;
        }
        uses_climate = true;
        break;
    case generation::GenerationStage::TerrainErosion:
    case generation::GenerationStage::FinalDrainage:
        if (!hasValidHydrology(snapshot, pixel_count))
        {
            return std::nullopt;
        }
        uses_hydrology = true;
        uses_climate = hasValidClimate(snapshot, pixel_count);
        break;
    case generation::GenerationStage::Complete:
    default:
        return std::nullopt;
    }

    GenerationMapImage image;
    image.width = snapshot.resolution;
    image.height = snapshot.resolution;
    image.rgba.resize(pixel_count);

    for (uint32_t index = 0U; index < pixel_count; ++index)
    {
        float const elevation = snapshot.elevation[index];
        GenerationMapRgba8 color = elevationColor(elevation);
        if (uses_hydrology)
        {
            color = waterColor(
                color,
                elevation,
                snapshot.water_level[index],
                snapshot.ocean_mask[index],
                snapshot.lake_mask[index],
                snapshot.river_mask[index]
            );
        }
        if (uses_climate)
        {
            color = blendColor(color, climateColor(snapshot.temperature[index], snapshot.precipitation[index]), 0.32F);
            if (uses_hydrology)
            {
                color = frozenSurfaceColor(
                    color,
                    snapshot.temperature[index],
                    snapshot.ocean_mask[index] != 0U
                );
            }
        }
        image.rgba[index] = shadedColor(color, snapshot.elevation, snapshot.resolution, index);
    }

    return image;
}

} // namespace client
