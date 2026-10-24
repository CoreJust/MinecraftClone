#include <client/S2MapPresentation.hpp>

#include <client/GenerationMapPresentation.hpp>
#include <client/render/TextRenderer.hpp>

#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace client {
namespace {

using Color = std::array<float, 3U>;
using Rgba = std::array<uint8_t, 4U>;

constexpr double WORLD_SIZE = static_cast<double>(generation::WORLD_SIZE_BLOCKS);
constexpr uint32_t WORLD_BLOCK_PERIOD = generation::WORLD_SIZE_BLOCKS;
constexpr uint32_t WORLD_PIXEL_COUNT = S2_WORLD_MAP_IMAGE_SIDE * S2_WORLD_MAP_IMAGE_SIDE;
constexpr uint32_t MINIMAP_PIXEL_COUNT = S2_MINIMAP_IMAGE_SIDE * S2_MINIMAP_IMAGE_SIDE;
constexpr std::array<Color, 6U> CLIMATE_COLORS{{
    {35.0F, 69.0F, 87.0F},
    {181.0F, 153.0F, 99.0F},
    {76.0F, 125.0F, 91.0F},
    {119.0F, 148.0F, 107.0F},
    {153.0F, 172.0F, 154.0F},
    {222.0F, 231.0F, 224.0F},
}};
constexpr Color PLATE_COLOR{213.0F, 164.0F, 237.0F};
constexpr Color WIND_COLOR{99.0F, 219.0F, 212.0F};
constexpr Color CURRENT_COLOR{220.0F, 238.0F, 247.0F};
constexpr Color VECTOR_OUTLINE_COLOR{17.0F, 42.0F, 53.0F};

struct Range final {
    float minimum = 0.0F;
    float maximum = 0.0F;
};

struct AxisSegment final {
    float source_start = 0.0F;
    float source_extent = 0.0F;
    float destination_fraction = 0.0F;
    float destination_extent = 0.0F;
};

struct WrappedAxisSegment final {
    uint32_t world_start = 0U;
    uint32_t local_start = 0U;
    uint32_t extent = 0U;
};

[[nodiscard]]
double wrapWorld(double coordinate) noexcept
{
    coordinate = std::fmod(coordinate, WORLD_SIZE);
    return coordinate < 0.0 ? coordinate + WORLD_SIZE : coordinate;
}

[[nodiscard]]
double wrapUnit(double coordinate) noexcept
{
    coordinate = std::fmod(coordinate, 1.0);
    return coordinate < 0.0 ? coordinate + 1.0 : coordinate;
}

[[nodiscard]]
bool validL0Source(shared::TerrainWorld const& world, shared::TerrainLodArtifact const& lod) noexcept
{
    auto const atlas = world.atlas();
    if (!atlas || !shared::isValidTerrainIdentity(world.identity())
        || lod.terrain_identity != world.identity() || lod.model_identity != shared::TERRAIN_LOD_MODEL
        || lod.level != shared::TerrainLodLevel::L0 || lod.extent != generation::WORLD_SIZE_BLOCKS
        || lod.origin_x != 0U || lod.origin_y != 0U
        || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return false;
    }
    uint32_t const expected_side = atlas->configuration.resolution + 1U;
    uint32_t const expected_spacing = generation::WORLD_SIZE_BLOCKS / atlas->configuration.resolution;
    return lod.spacing == expected_spacing && lod.side == expected_side
        && lod.nodes.size() == static_cast<uint64_t>(expected_side) * expected_side;
}

template <typename TValue>
[[nodiscard]]
bool hasFiniteField(std::vector<TValue> const& field, size_t const count) noexcept
{
    if (field.size() != count) {
        return false;
    }
    if constexpr (std::is_floating_point_v<TValue>) {
        return std::all_of(field.begin(), field.end(), [](TValue const value) { return std::isfinite(value); });
    }
    return true;
}

[[nodiscard]]
bool hasBinaryMask(std::vector<uint8_t> const& field, size_t const count) noexcept
{
    return field.size() == count
        && std::all_of(field.begin(), field.end(), [](uint8_t const value) { return value <= 1U; });
}

template <typename TValue>
[[nodiscard]]
Range fieldRange(std::vector<TValue> const& field) noexcept
{
    auto const [minimum, maximum] = std::minmax_element(field.begin(), field.end());
    return {static_cast<float>(*minimum), static_cast<float>(*maximum)};
}

[[nodiscard]]
Color materialColor(shared::Block const block) noexcept
{
    switch (block) {
    case shared::Block::Air: return {189.0F, 217.0F, 237.0F};
    case shared::Block::Stone:
    case shared::Block::Bedrock: return {128.0F, 128.0F, 128.0F};
    case shared::Block::Grass: return {92.0F, 147.0F, 69.0F};
    case shared::Block::Dirt: return {124.0F, 97.0F, 66.0F};
    case shared::Block::Snow: return {244.0F, 247.0F, 250.0F};
    case shared::Block::Ice: return {128.0F, 201.0F, 214.0F};
    }
    return {128.0F, 128.0F, 128.0F};
}

[[nodiscard]]
Color ramp(float value, std::span<std::pair<float, Color> const> stops) noexcept
{
    if (value <= stops.front().first) {
        return stops.front().second;
    }
    for (size_t index = 1U; index < stops.size(); ++index) {
        auto const& lower = stops[index - 1U];
        auto const& upper = stops[index];
        if (value <= upper.first) {
            float const amount = (value - lower.first) / (upper.first - lower.first);
            Color result{};
            for (size_t channel = 0U; channel < result.size(); ++channel) {
                result[channel] = lower.second[channel] + (upper.second[channel] - lower.second[channel]) * amount;
            }
            return result;
        }
    }
    return stops.back().second;
}

[[nodiscard]]
float niceStep(float value) noexcept
{
    if (!(value > 0.0F) || !std::isfinite(value)) {
        return 1.0F;
    }
    float const power = std::pow(10.0F, std::floor(std::log10(value)));
    float const fraction = value / power;
    float const multiplier = fraction < 1.5F ? 1.0F : (fraction < 3.5F ? 2.0F : (fraction < 7.5F ? 5.0F : 10.0F));
    return multiplier * power;
}

[[nodiscard]]
Color elevationColor(float value, Range const range) noexcept
{
    if (value < 0.0F) {
        static constexpr std::array<std::pair<float, Color>, 2U> SEA_STOPS{{
            {0.0F, {24.0F, 51.0F, 70.0F}},
            {1.0F, {101.0F, 150.0F, 155.0F}},
        }};
        float const minimum = std::min(-0.01F, range.minimum);
        float const position = std::clamp((value - minimum) / std::max(0.01F, -minimum), 0.0F, 1.0F);
        return ramp(position, SEA_STOPS);
    }
    static constexpr std::array<std::pair<float, Color>, 4U> LAND_STOPS{{
        {0.0F, {126.0F, 151.0F, 119.0F}},
        {0.35F, {177.0F, 170.0F, 123.0F}},
        {0.7F, {199.0F, 184.0F, 153.0F}},
        {1.0F, {237.0F, 234.0F, 217.0F}},
    }};
    return ramp(value / std::max(0.01F, range.maximum), LAND_STOPS);
}

[[nodiscard]]
Color temperatureColor(float const temperature) noexcept
{
    static constexpr std::array<std::pair<float, Color>, 5U> STOPS{{
        {-40.0F, {42.0F, 78.0F, 116.0F}},
        {-20.0F, {99.0F, 153.0F, 180.0F}},
        {0.0F, {202.0F, 215.0F, 204.0F}},
        {15.0F, {222.0F, 187.0F, 125.0F}},
        {30.0F, {205.0F, 120.0F, 83.0F}},
    }};
    return ramp(temperature, STOPS);
}

[[nodiscard]]
Color heatColor(S2WorldMapLayer const layer, float const value, Range const range) noexcept
{
    if (layer == S2WorldMapLayer::Temperature) {
        return temperatureColor(value);
    }
    float normalized = (value - range.minimum) / std::max(1.0e-6F, range.maximum - range.minimum);
    normalized = std::clamp(normalized, 0.0F, 1.0F);
    if (layer == S2WorldMapLayer::Precipitation) {
        normalized = std::log1p(std::max(0.0F, value - range.minimum))
            / std::log1p(std::max(1.0e-6F, range.maximum - range.minimum));
    }
    static constexpr std::array<std::pair<float, Color>, 3U> MOISTURE_STOPS{{
        {0.0F, {209.0F, 189.0F, 137.0F}},
        {0.5F, {109.0F, 183.0F, 158.0F}},
        {1.0F, {31.0F, 137.0F, 125.0F}},
    }};
    static constexpr std::array<std::pair<float, Color>, 3U> PRECIPITATION_STOPS{{
        {0.0F, {220.0F, 213.0F, 235.0F}},
        {0.5F, {163.0F, 130.0F, 199.0F}},
        {1.0F, {99.0F, 73.0F, 170.0F}},
    }};
    return layer == S2WorldMapLayer::Moisture
        ? ramp(normalized, MOISTURE_STOPS)
        : ramp(normalized, PRECIPITATION_STOPS);
}

void blend(Color& base, Color const tint, float const alpha) noexcept
{
    for (size_t channel = 0U; channel < base.size(); ++channel) {
        base[channel] = std::clamp(base[channel] + (tint[channel] - base[channel]) * alpha, 0.0F, 255.0F);
    }
}

void setPixel(S2MapImage& image, int32_t const x, int32_t const y, Color const color) noexcept
{
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= image.width || static_cast<uint32_t>(y) >= image.height) {
        return;
    }
    Rgba& pixel = image.rgba[static_cast<size_t>(y) * image.width + static_cast<uint32_t>(x)];
    pixel = {
        static_cast<uint8_t>(std::clamp(std::lround(color[0]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[1]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[2]), 0L, 255L)),
        255U,
    };
}

void line(S2MapImage& image, int32_t x0, int32_t y0, int32_t x1, int32_t y1, Color const color) noexcept
{
    int32_t const dx = std::abs(x1 - x0);
    int32_t const sx = x0 < x1 ? 1 : -1;
    int32_t const dy = -std::abs(y1 - y0);
    int32_t const sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    while (true) {
        setPixel(image, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t const twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void outlinedLine(S2MapImage& image, int32_t const x0, int32_t const y0, int32_t const x1, int32_t const y1,
    Color const color) noexcept
{
    for (int32_t offset = -1; offset <= 1; ++offset) {
        line(image, x0 + offset, y0, x1 + offset, y1, VECTOR_OUTLINE_COLOR);
        line(image, x0, y0 + offset, x1, y1 + offset, VECTOR_OUTLINE_COLOR);
    }
    line(image, x0, y0, x1, y1, color);
}

void drawArrow(
    S2MapImage& image,
    int32_t const x,
    int32_t const y,
    float const u,
    float const v,
    Color const color,
    bool const double_head = false
) noexcept
{
    float const magnitude = std::hypot(u, v);
    if (!std::isfinite(magnitude)) {
        return;
    }
    if (magnitude < 1.0e-6F) {
        setPixel(image, x, y, color);
        setPixel(image, x + 1, y, color);
        setPixel(image, x, y + 1, color);
        setPixel(image, x + 1, y + 1, color);
        return;
    }
    float const dx = u / magnitude;
    float const dy = v / magnitude;
    constexpr int32_t HALF_LENGTH = 4;
    int32_t const start_x = static_cast<int32_t>(std::lround(x - dx * HALF_LENGTH));
    int32_t const start_y = static_cast<int32_t>(std::lround(y - dy * HALF_LENGTH));
    int32_t const end_x = static_cast<int32_t>(std::lround(x + dx * HALF_LENGTH));
    int32_t const end_y = static_cast<int32_t>(std::lround(y + dy * HALF_LENGTH));
    outlinedLine(image, start_x, start_y, end_x, end_y, color);
    float const scale = static_cast<float>(image.width) / 1'280.0F;
    std::array<float, 2U> const head_offsets{0.0F, 4.0F * scale};
    for (size_t head = 0U; head < (double_head ? head_offsets.size() : 1U); ++head) {
        float const offset = head_offsets[head];
        int32_t const tip_x = static_cast<int32_t>(std::lround(end_x - dx * offset));
        int32_t const tip_y = static_cast<int32_t>(std::lround(end_y - dy * offset));
        float const wing_length = 4.0F * scale;
        float const wing_width = 3.0F * scale;
        int32_t const wing1_x = static_cast<int32_t>(std::lround(tip_x - dx * wing_length - dy * wing_width));
        int32_t const wing1_y = static_cast<int32_t>(std::lround(tip_y - dy * wing_length + dx * wing_width));
        int32_t const wing2_x = static_cast<int32_t>(std::lround(tip_x - dx * wing_length + dy * wing_width));
        int32_t const wing2_y = static_cast<int32_t>(std::lround(tip_y - dy * wing_length - dx * wing_width));
        outlinedLine(image, tip_x, tip_y, wing1_x, wing1_y, color);
        outlinedLine(image, tip_x, tip_y, wing2_x, wing2_y, color);
    }
}

[[nodiscard]]
std::vector<AxisSegment> splitAxis(float start, float extent)
{
    std::vector<AxisSegment> result;
    if (extent >= 1.0F) {
        result.push_back({.source_start = 0.0F, .source_extent = 1.0F,
            .destination_fraction = 0.0F, .destination_extent = 1.0F});
        return result;
    }
    start = static_cast<float>(wrapUnit(start));
    float const first_extent = std::min(extent, 1.0F - start);
    result.push_back({.source_start = start, .source_extent = first_extent,
        .destination_fraction = 0.0F, .destination_extent = first_extent / extent});
    float const remainder = extent - first_extent;
    if (remainder > 1.0e-6F) {
        result.push_back({.source_start = 0.0F, .source_extent = remainder,
            .destination_fraction = first_extent / extent, .destination_extent = remainder / extent});
    }
    return result;
}

[[nodiscard]]
std::array<WrappedAxisSegment, 2U> splitWrappedAxis(uint32_t origin, uint32_t extent, uint32_t& count) noexcept
{
    std::array<WrappedAxisSegment, 2U> result{};
    count = 0U;
    uint32_t const first_extent = std::min(extent, WORLD_BLOCK_PERIOD - origin);
    result[count++] = {.world_start = origin, .local_start = 0U, .extent = first_extent};
    uint32_t const remaining = extent - first_extent;
    if (remaining > 0U) {
        result[count++] = {.world_start = 0U, .local_start = first_extent, .extent = remaining};
    }
    return result;
}

[[nodiscard]]
Color minimapMaterialColor(shared::TerrainLodSample const& sample) noexcept
{
    if (sample.water_present) {
        return sample.temperature <= -10.0
            ? Color{128.0F, 201.0F, 214.0F}
            : Color{64.0F, 124.0F, 157.0F};
    }
    return materialColor(sample.top_material);
}

[[nodiscard]]
bool validLocalLod(shared::TerrainLodArtifact const& lod) noexcept
{
    uint64_t const expected_nodes = static_cast<uint64_t>(lod.side) * lod.side;
    bool const valid_spacing = lod.level == shared::TerrainLodLevel::L1
        && (lod.spacing == 32U || lod.spacing == 64U);
    return shared::isValidTerrainIdentity(lod.terrain_identity)
        && lod.model_identity == shared::TERRAIN_LOD_MODEL
        && valid_spacing
        && lod.spacing != 0U && lod.extent >= S2_MINIMAP_WORLD_EXTENT
        && lod.extent <= generation::WORLD_SIZE_BLOCKS && lod.extent % lod.spacing == 0U
        && lod.origin_x % lod.spacing == 0U && lod.origin_y % lod.spacing == 0U
        && lod.origin_x < generation::WORLD_SIZE_BLOCKS && lod.origin_y < generation::WORLD_SIZE_BLOCKS
        && lod.side == lod.extent / lod.spacing + 1U
        && lod.side <= S2_MINIMAP_IMAGE_SIDE * 2U + 1U && expected_nodes <= shared::TERRAIN_LOD_MAX_NODES
        && lod.nodes.size() == expected_nodes;
}

[[nodiscard]]
TextColor textColor(Color const color) noexcept
{
    return {
        color[0] / 255.0F,
        color[1] / 255.0F,
        color[2] / 255.0F,
        1.0F,
    };
}

void submitMapText(
    TextRenderer& output,
    std::string_view const value,
    Color const color,
    float const x,
    float const y,
    float const dpi_scale,
    bool const outlined = false,
    float const text_scale = 0.5F
)
{
    TextColor const foreground = textColor(color);
    TextPlacement const placement{
        .position = {x * dpi_scale, y * dpi_scale, 0.0F},
        .scale = text_scale * dpi_scale,
    };
    if (outlined) {
        TextColor const outline = textColor({12.0F, 29.0F, 36.0F});
        for (auto const [dx, dy] : std::array<std::array<float, 2U>, 8U>{{
            {-1.0F, 0.0F}, {1.0F, 0.0F}, {0.0F, -1.0F}, {0.0F, 1.0F},
            {-1.0F, -1.0F}, {1.0F, -1.0F}, {-1.0F, 1.0F}, {1.0F, 1.0F},
        }}) {
            TextPlacement const outline_placement{
                .position = {(x + dx) * dpi_scale, (y + dy) * dpi_scale, 0.0F},
                .scale = text_scale * dpi_scale,
            };
            static_cast<void>(output.submit(value, outline, outline_placement));
        }
    }
    static_cast<void>(output.submit(value, foreground, placement));
}

[[nodiscard]]
std::string fixedText(float const value, int const places)
{
    if (!std::isfinite(value)) {
        return "n/a";
    }
    std::array<char, 64U> buffer{};
    auto const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::fixed, places);
    if (result.ec != std::errc{}) {
        return "n/a";
    }
    return {buffer.data(), result.ptr};
}

[[nodiscard]]
std::string numberText(float const value, int const places = 2)
{
    if (!std::isfinite(value)) {
        return "n/a";
    }
    float const factor = std::pow(10.0F, static_cast<float>(places));
    float const rounded = std::floor(value * factor + 0.5F) / factor;
    if (rounded == 0.0F) {
        return "0";
    }
    std::string result = fixedText(rounded, places);
    if (result.find('.') != std::string::npos) {
        while (!result.empty() && result.back() == '0') {
            result.pop_back();
        }
        if (!result.empty() && result.back() == '.') {
            result.pop_back();
        }
    }
    return result;
}

[[nodiscard]]
std::string rangeText(S2MapImage const& image, S2WorldMapLayer const layer)
{
    size_t const index = static_cast<size_t>(layer);
    if (!image.has_numeric_range[index]) {
        return "n/a";
    }
    auto const& range = image.layer_ranges[index];
    int const minimum_places = range[0] > 0.0F && range[0] < 0.01F ? 4 : 2;
    int const maximum_places = range[1] < 1.0F ? 2 : 1;
    return numberText(range[0], minimum_places) + ".." + numberText(range[1], maximum_places);
}

[[nodiscard]]
std::string fitMapText(std::string value, float const width, float const dpi_scale)
{
    size_t const maximum_characters = static_cast<size_t>(std::max(1.0F, std::floor(width / (8.0F * dpi_scale))));
    if (value.size() > maximum_characters) {
        if (maximum_characters <= 3U) {
            value.resize(maximum_characters);
        } else {
            value.resize(maximum_characters - 3U);
            value += "...";
        }
    }
    return value;
}

[[nodiscard]]
std::optional<size_t> mapSampleIndex(
    shared::TerrainWorld const* const world,
    S2MapImage const* const image,
    double const player_x,
    double const player_y
)
{
    if (!world || !image || image->source_identity != world->identity()
        || !std::isfinite(player_x) || !std::isfinite(player_y)) {
        return std::nullopt;
    }
    auto const atlas = world->atlas();
    if (!atlas || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return std::nullopt;
    }
    uint32_t const resolution = atlas->configuration.resolution;
    uint32_t const x = static_cast<uint32_t>(wrapWorld(player_x) * resolution / WORLD_SIZE);
    uint32_t const y = static_cast<uint32_t>(wrapWorld(player_y) * resolution / WORLD_SIZE);
    return static_cast<size_t>(y % resolution) * resolution + (x % resolution);
}

[[nodiscard]]
float sanitizedDpi(float const dpi_scale) noexcept
{
    return std::isfinite(dpi_scale) && dpi_scale > 0.0F
        ? std::clamp(dpi_scale, 0.25F, 8.0F)
        : 1.0F;
}

} // namespace

std::optional<S2MapImage> rasterizeS2WorldMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& world_lod,
    S2WorldMapState const& state
)
{
    if (!validL0Source(world, world_lod)) {
        return std::nullopt;
    }
    auto const atlas = world.atlas();
    uint32_t const resolution = atlas->configuration.resolution;
    size_t const count = static_cast<size_t>(resolution) * resolution;
    S2MapImage image{
        .width = S2_WORLD_MAP_IMAGE_SIDE,
        .height = S2_WORLD_MAP_IMAGE_SIDE,
        .source_identity = world.identity(),
        .source_model_identity = world_lod.model_identity,
    };
    image.rgba.resize(WORLD_PIXEL_COUNT);

    bool const elevation_valid = hasFiniteField(atlas->elevation, count);
    bool const temperature_valid = hasFiniteField(atlas->temperature, count);
    bool const precipitation_valid = hasFiniteField(atlas->precipitation, count);
    bool const moisture_valid = hasFiniteField(atlas->moisture, count);
    bool const climate_zone_valid = atlas->climate_zone.size() == count
        && std::all_of(atlas->climate_zone.begin(), atlas->climate_zone.end(), [](uint8_t zone) { return zone < 6U; });
    bool const plate_ids_valid = atlas->plate_id.size() == count
        && std::all_of(atlas->plate_id.begin(), atlas->plate_id.end(), [](int16_t id) { return id >= 0; });
    bool const plate_vectors_valid = hasFiniteField(atlas->plate_vx, count) && hasFiniteField(atlas->plate_vy, count);
    bool const wind_valid = hasFiniteField(atlas->wind_u, count) && hasFiniteField(atlas->wind_v, count);
    bool const current_valid = hasFiniteField(atlas->current_u, count) && hasFiniteField(atlas->current_v, count);
    bool const water_valid = hasBinaryMask(atlas->ocean_mask, count) && hasBinaryMask(atlas->lake_mask, count)
        && hasBinaryMask(atlas->river_mask, count) && hasFiniteField(atlas->water_level, count);
    auto const display_ice_mask = makeGenerationMapDisplayIceMask(
        std::span<uint8_t const>(atlas->climate_zone),
        std::span<uint8_t const>(atlas->ocean_mask),
        std::span<uint8_t const>(atlas->lake_mask),
        std::span<float const>(atlas->temperature)
    );
    std::span<uint8_t const> display_ice_surface_kind;
    if (display_ice_mask) {
        display_ice_surface_kind = std::span<uint8_t const>(display_ice_mask->surface_kind);
    }
    auto const generation_base = rasterizeGenerationMap(*atlas, display_ice_surface_kind);
    image.has_display_ice_mask = display_ice_mask.has_value();
    if (display_ice_mask) {
        image.ice_snow_count = static_cast<uint32_t>(std::count(
            display_ice_mask->surface_kind.begin(), display_ice_mask->surface_kind.end(), GENERATION_MAP_ICE_SNOW));
        image.ice_water_count = static_cast<uint32_t>(std::count(
            display_ice_mask->surface_kind.begin(), display_ice_mask->surface_kind.end(), GENERATION_MAP_ICE_WATER));
    }
    image.available_layers = {
        plate_ids_valid && plate_vectors_valid,
        elevation_valid,
        temperature_valid,
        climate_zone_valid,
        wind_valid,
        current_valid && water_valid,
        moisture_valid,
        precipitation_valid,
    };

    auto const layer = [&image](S2WorldMapLayer value) {
        return image.available_layers[static_cast<size_t>(value)];
    };
    Range const elevation_range = elevation_valid ? fieldRange(atlas->elevation) : Range{};
    Range const temperature_range{-40.0F, 30.0F};
    Range const moisture_range = moisture_valid ? fieldRange(atlas->moisture) : Range{};
    Range const precipitation_range = precipitation_valid ? fieldRange(atlas->precipitation) : Range{};
    if (image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]) {
        Range const plate_range = fieldRange(atlas->plate_id);
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Plates)] = {plate_range.minimum, plate_range.maximum};
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Plates)] = true;
    }
    if (elevation_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)] = {
            elevation_range.minimum, elevation_range.maximum,
        };
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Heights)] = true;
    }
    if (temperature_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Temperature)] = {-40.0F, 30.0F};
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Temperature)] = true;
    }
    if (climate_zone_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Biomes)] = {0.0F, 5.0F};
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Biomes)] = true;
    }
    if (moisture_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Moisture)] = {
            moisture_range.minimum, moisture_range.maximum,
        };
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Moisture)] = true;
    }
    if (precipitation_valid) {
        image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Precipitation)] = {
            precipitation_range.minimum, precipitation_range.maximum,
        };
        image.has_numeric_range[static_cast<size_t>(S2WorldMapLayer::Precipitation)] = true;
    }
    float const elevation_extent = std::max(0.001F, elevation_range.maximum - elevation_range.minimum);
    uint32_t heat_count = 0U;
    for (S2WorldMapLayer const value : {
        S2WorldMapLayer::Temperature, S2WorldMapLayer::Moisture, S2WorldMapLayer::Precipitation,
    }) {
        heat_count += state.layers[static_cast<size_t>(value)] && layer(value);
    }

    auto const atlasIndex = [resolution](uint32_t const x, uint32_t const y) {
        return static_cast<size_t>(y % resolution) * resolution + (x % resolution);
    };
    auto const rootSample = [&world_lod](uint32_t const x, uint32_t const y) -> shared::TerrainLodSample const& {
        return world_lod.nodes[static_cast<size_t>(y) * world_lod.side + x].sample;
    };
    auto const waterAt = [&](uint32_t const x, uint32_t const y) {
        size_t const index = atlasIndex(x, y);
        return water_valid && (atlas->ocean_mask[index] != 0U || atlas->lake_mask[index] != 0U
            || atlas->river_mask[index] != 0U);
    };

    for (uint32_t y = 0U; y < image.height; ++y) {
        uint32_t const source_y = y * resolution / image.height;
        for (uint32_t x = 0U; x < image.width; ++x) {
            uint32_t const source_x = x * resolution / image.width;
            size_t const index = atlasIndex(source_x, source_y);
            shared::TerrainLodSample const& sample = rootSample(source_x, source_y);
            bool const water = sample.water_present || waterAt(source_x, source_y);
            uint8_t const ice_kind = display_ice_mask && index < display_ice_mask->surface_kind.size()
                ? display_ice_mask->surface_kind[index]
                : GENERATION_MAP_ICE_NONE;
            bool const ice_water = water && ice_kind == GENERATION_MAP_ICE_WATER;
            bool const snow_land = !water && ice_kind == GENERATION_MAP_ICE_SNOW;
            Color color;
            if (generation_base && index < generation_base->rgba.size()) {
                auto const& pixel = generation_base->rgba[index];
                color = {static_cast<float>(pixel[0]), static_cast<float>(pixel[1]), static_cast<float>(pixel[2])};
            } else {
                color = water
                    ? (ice_water ? Color{203.0F, 227.0F, 231.0F} : Color{35.0F, 69.0F, 87.0F})
                    : materialColor(sample.top_material);
                if (snow_land) {
                    blend(color, {236.0F, 242.0F, 235.0F}, 0.30F);
                }
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Biomes)] && climate_zone_valid && !water) {
                color = CLIMATE_COLORS[atlas->climate_zone[index]];
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Heights)] && elevation_valid) {
                uint32_t const west = (source_x + resolution - 1U) % resolution;
                uint32_t const north = (source_y + resolution - 1U) % resolution;
                float const slope = (atlas->elevation[atlasIndex(source_x + 1U, source_y)]
                    - atlas->elevation[atlasIndex(west, source_y)])
                    + 0.65F * (atlas->elevation[atlasIndex(source_x, source_y + 1U)]
                        - atlas->elevation[atlasIndex(source_x, north)]);
                float const relief = std::clamp(0.84F
                    + (atlas->elevation[index] - elevation_range.minimum) / elevation_extent * 0.30F
                    - slope * 0.08F, 0.52F, 1.28F);
                for (float& channel : color) {
                    channel *= relief;
                }
                float const contour_step = elevation_extent / 12.0F;
                if (contour_step > 0.0001F) {
                    float const band = std::floor(atlas->elevation[index] / contour_step);
                    if (band != std::floor(atlas->elevation[atlasIndex(source_x + 1U, source_y)] / contour_step)
                        || band != std::floor(atlas->elevation[atlasIndex(source_x, source_y + 1U)] / contour_step)) {
                        blend(color, water ? Color{105.0F, 151.0F, 169.0F} : Color{235.0F, 222.0F, 186.0F},
                            water ? 0.19F : 0.29F);
                    }
                }
            }
            if (water && !ice_water && water_valid) {
                float const depth = std::clamp(atlas->water_level[index] - atlas->elevation[index], 0.0F, 4.0F);
                blend(color, {20.0F, 45.0F, 60.0F}, std::min(0.40F, depth * 0.07F));
            }

            if (heat_count > 0U) {
                Color tint{};
                auto const addTint = [&tint](Color const value) {
                    for (size_t channel = 0U; channel < tint.size(); ++channel) {
                        tint[channel] += value[channel];
                    }
                };
                if (state.layers[static_cast<size_t>(S2WorldMapLayer::Temperature)] && temperature_valid) {
                    addTint(heatColor(S2WorldMapLayer::Temperature, atlas->temperature[index], temperature_range));
                }
                if (state.layers[static_cast<size_t>(S2WorldMapLayer::Moisture)] && moisture_valid) {
                    addTint(heatColor(S2WorldMapLayer::Moisture, atlas->moisture[index], moisture_range));
                }
                if (state.layers[static_cast<size_t>(S2WorldMapLayer::Precipitation)] && precipitation_valid) {
                    addTint(heatColor(S2WorldMapLayer::Precipitation, atlas->precipitation[index], precipitation_range));
                }
                for (float& channel : tint) {
                    channel /= static_cast<float>(heat_count);
                }
                color = tint;
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)] && plate_ids_valid) {
                int16_t const plate = atlas->plate_id[index];
                bool const boundary = plate != atlas->plate_id[atlasIndex(source_x + 1U, source_y)]
                    || plate != atlas->plate_id[atlasIndex(source_x, source_y + 1U)];
                if (boundary) {
                    blend(color, PLATE_COLOR, 0.95F);
                }
            }
            setPixel(image, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
        }
    }

    uint32_t const vector_step = std::max(8U, image.width / 18U);
    for (uint32_t y = vector_step / 2U; y < image.height; y += vector_step) {
        uint32_t const source_y = y * resolution / image.height;
        for (uint32_t x = vector_step / 2U; x < image.width; x += vector_step) {
            uint32_t const source_x = x * resolution / image.width;
            size_t const index = atlasIndex(source_x, source_y);
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Wind)] && wind_valid) {
                drawArrow(image, static_cast<int32_t>(x), static_cast<int32_t>(y),
                    atlas->wind_u[index], atlas->wind_v[index], WIND_COLOR);
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Currents)] && current_valid && waterAt(source_x, source_y)) {
                drawArrow(image, static_cast<int32_t>(x), static_cast<int32_t>(y),
                    atlas->current_u[index], atlas->current_v[index], CURRENT_COLOR, true);
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)] && plate_vectors_valid) {
                drawArrow(image, static_cast<int32_t>(x), static_cast<int32_t>(y),
                    atlas->plate_vx[index], atlas->plate_vy[index], PLATE_COLOR);
            }
        }
    }
    return image;
}

std::shared_ptr<shared::TerrainLodArtifact const> buildS2MinimapLod(
    shared::TerrainWorld const& world,
    double const player_x,
    double const player_y,
    std::stop_token const& cancellation
)
{
    auto const atlas = world.atlas();
    if (!atlas || !shared::isValidTerrainIdentity(world.identity())
        || !std::isfinite(player_x) || !std::isfinite(player_y)
        || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return {};
    }
    uint32_t const spacing = shared::terrainLodSpacing(
        atlas->configuration.resolution, shared::TerrainLodLevel::L1);
    uint32_t const origin_x = static_cast<uint32_t>(wrapWorld(player_x - S2_MINIMAP_WORLD_EXTENT / 2.0))
        / spacing * spacing;
    uint32_t const origin_y = static_cast<uint32_t>(wrapWorld(player_y - S2_MINIMAP_WORLD_EXTENT / 2.0))
        / spacing * spacing;
    uint32_t x_segment_count = 0U;
    uint32_t y_segment_count = 0U;
    auto const x_segments = splitWrappedAxis(origin_x, S2_MINIMAP_WORLD_EXTENT, x_segment_count);
    auto const y_segments = splitWrappedAxis(origin_y, S2_MINIMAP_WORLD_EXTENT, y_segment_count);
    auto result = std::make_shared<shared::TerrainLodArtifact>();
    result->terrain_identity = world.identity();
    result->level = shared::TerrainLodLevel::L1;
    result->spacing = spacing;
    result->origin_x = origin_x;
    result->origin_y = origin_y;
    result->extent = S2_MINIMAP_WORLD_EXTENT;
    result->side = S2_MINIMAP_WORLD_EXTENT / spacing + 1U;
    result->nodes.resize(static_cast<size_t>(result->side) * result->side);

    for (uint32_t y_segment_index = 0U; y_segment_index < y_segment_count; ++y_segment_index) {
        WrappedAxisSegment const& y_segment = y_segments[y_segment_index];
        for (uint32_t x_segment_index = 0U; x_segment_index < x_segment_count; ++x_segment_index) {
            WrappedAxisSegment const& x_segment = x_segments[x_segment_index];
            uint32_t const patch_extent = std::max(x_segment.extent, y_segment.extent);
            uint32_t patch_origin_x = x_segment.world_start;
            uint32_t patch_origin_y = y_segment.world_start;
            if (patch_origin_x + patch_extent > WORLD_BLOCK_PERIOD) {
                patch_origin_x = WORLD_BLOCK_PERIOD - patch_extent;
            }
            if (patch_origin_y + patch_extent > WORLD_BLOCK_PERIOD) {
                patch_origin_y = WORLD_BLOCK_PERIOD - patch_extent;
            }
            auto const patch = shared::buildTerrainLod(
                world,
                shared::TerrainLodLevel::L1,
                patch_origin_x,
                patch_origin_y,
                patch_extent,
                cancellation
            );
            if (!patch) {
                return {};
            }
            uint32_t const source_x = (x_segment.world_start - patch_origin_x) / spacing;
            uint32_t const source_y = (y_segment.world_start - patch_origin_y) / spacing;
            uint32_t const destination_x = x_segment.local_start / spacing;
            uint32_t const destination_y = y_segment.local_start / spacing;
            uint32_t const columns = x_segment.extent / spacing + 1U;
            uint32_t const rows = y_segment.extent / spacing + 1U;
            for (uint32_t row = 0U; row < rows; ++row) {
                for (uint32_t column = 0U; column < columns; ++column) {
                    uint32_t const local_x = destination_x + column;
                    uint32_t const local_y = destination_y + row;
                    shared::TerrainLodNode node = patch->nodes[
                        static_cast<size_t>(source_y + row) * patch->side + source_x + column];
                    node.key.x = (origin_x + local_x * spacing) % WORLD_BLOCK_PERIOD;
                    node.key.y = (origin_y + local_y * spacing) % WORLD_BLOCK_PERIOD;
                    result->nodes[static_cast<size_t>(local_y) * result->side + local_x] = std::move(node);
                }
            }
        }
    }
    return result;
}

std::optional<S2MapImage> rasterizeS2Minimap(shared::TerrainLodArtifact const& local_lod)
{
    if (!validLocalLod(local_lod)) {
        return std::nullopt;
    }
    S2MapImage image{
        .width = S2_MINIMAP_IMAGE_SIDE,
        .height = S2_MINIMAP_IMAGE_SIDE,
        .source_identity = local_lod.terrain_identity,
        .source_model_identity = local_lod.model_identity,
    };
    image.rgba.resize(MINIMAP_PIXEL_COUNT);
    for (uint32_t y = 0U; y < image.height; ++y) {
        uint32_t const row = std::min(
            y * local_lod.extent / image.height / local_lod.spacing, local_lod.side - 2U);
        for (uint32_t x = 0U; x < image.width; ++x) {
            uint32_t const column = std::min(
                x * local_lod.extent / image.width / local_lod.spacing, local_lod.side - 2U);
            size_t const index = static_cast<size_t>(row) * local_lod.side + column;
            Color color = minimapMaterialColor(local_lod.nodes[index].sample);
            if (!local_lod.nodes[index].sample.water_present) {
                uint32_t const west = column == 0U ? column : column - 1U;
                uint32_t const east = std::min(column + 1U, local_lod.side - 1U);
                uint32_t const north = row == 0U ? row : row - 1U;
                uint32_t const south = std::min(row + 1U, local_lod.side - 1U);
                double const west_height = local_lod.nodes[static_cast<size_t>(row) * local_lod.side + west].sample.height;
                double const east_height = local_lod.nodes[static_cast<size_t>(row) * local_lod.side + east].sample.height;
                double const north_height = local_lod.nodes[static_cast<size_t>(north) * local_lod.side + column].sample.height;
                double const south_height = local_lod.nodes[static_cast<size_t>(south) * local_lod.side + column].sample.height;
                double const relief = 0.92 + (west_height - east_height + 0.65 * (north_height - south_height)) * 0.0015;
                float const shade = static_cast<float>(std::clamp(relief, 0.75, 1.10));
                for (float& channel : color) {
                    channel *= shade;
                }
            }
            setPixel(image, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
        }
    }
    for (uint32_t coordinate = 0U; coordinate < image.width; ++coordinate) {
        setPixel(image, static_cast<int32_t>(coordinate), 0, {20.0F, 36.0F, 44.0F});
        setPixel(image, static_cast<int32_t>(coordinate), static_cast<int32_t>(image.height - 1U), {20.0F, 36.0F, 44.0F});
        setPixel(image, 0, static_cast<int32_t>(coordinate), {20.0F, 36.0F, 44.0F});
        setPixel(image, static_cast<int32_t>(image.width - 1U), static_cast<int32_t>(coordinate), {20.0F, 36.0F, 44.0F});
    }
    return image;
}

S2MinimapMarker layoutS2MinimapMarker(
    shared::TerrainLodArtifact const& local_lod,
    double const player_x,
    double const player_y,
    double const heading_degrees,
    std::array<float, 4U> const& minimap_bounds
) noexcept
{
    if (!validLocalLod(local_lod) || !std::isfinite(player_x) || !std::isfinite(player_y)
        || !std::isfinite(heading_degrees) || minimap_bounds[2] <= 0.0F || minimap_bounds[3] <= 0.0F) {
        return {};
    }
    double const local_x = std::fmod(std::fmod(player_x - local_lod.origin_x, WORLD_SIZE) + WORLD_SIZE, WORLD_SIZE);
    double const local_y = std::fmod(std::fmod(player_y - local_lod.origin_y, WORLD_SIZE) + WORLD_SIZE, WORLD_SIZE);
    if (local_x > local_lod.extent || local_y > local_lod.extent) {
        return {};
    }
    double heading = std::fmod(heading_degrees, 360.0);
    if (heading < 0.0) {
        heading += 360.0;
    }
    uint32_t const octant = static_cast<uint32_t>(std::lround(heading / 45.0)) % 8U;
    constexpr std::array<char, 8U> DIRECTION_GLYPHS{'^', '/', '>', '\\', 'v', '/', '<', '\\'};
    return {
        .x = minimap_bounds[0] + static_cast<float>(local_x / local_lod.extent) * minimap_bounds[2],
        .y = minimap_bounds[1] + static_cast<float>(local_y / local_lod.extent) * minimap_bounds[3],
        .heading_degrees = heading,
        .direction_glyph = DIRECTION_GLYPHS[octant],
        .visible = true,
    };
}

S2MapLayout layoutS2Map(
    S2WorldMapState const& state,
    uint32_t const width,
    uint32_t const height,
    bool const map_open,
    bool const show_minimap,
    float const requested_dpi_scale
) noexcept
{
    S2MapLayout result;
    if (width == 0U || height == 0U) {
        return result;
    }
    if (map_open) {
        if (state.overview) {
            float const side = static_cast<float>(std::min(width, height));
            result.world_draws[0] = {
                .image = 0U,
                .source = {0.0F, 0.0F, 1.0F, 1.0F},
                .destination = {(static_cast<float>(width) - side) * 0.5F,
                    (static_cast<float>(height) - side) * 0.5F, side, side},
            };
            result.world_draw_count = 1U;
            return result;
        }
        float const side = static_cast<float>(std::max(width, height))
            * static_cast<float>(std::clamp(state.zoom, 1.0, 6.0));
        float const source_width = static_cast<float>(width) / side;
        float const source_height = static_cast<float>(height) / side;
        auto const x_segments = splitAxis(
            static_cast<float>(state.center_x / WORLD_SIZE - source_width * 0.5), source_width);
        auto const y_segments = splitAxis(
            static_cast<float>(state.center_y / WORLD_SIZE - source_height * 0.5), source_height);
        for (AxisSegment const& y_segment : y_segments) {
            for (AxisSegment const& x_segment : x_segments) {
                if (result.world_draw_count >= result.world_draws.size()) {
                    break;
                }
                result.world_draws[result.world_draw_count++] = {
                    .image = 0U,
                    .source = {x_segment.source_start, y_segment.source_start,
                        x_segment.source_extent, y_segment.source_extent},
                    .destination = {
                        x_segment.destination_fraction * static_cast<float>(width),
                        y_segment.destination_fraction * static_cast<float>(height),
                        x_segment.destination_extent * static_cast<float>(width),
                        y_segment.destination_extent * static_cast<float>(height),
                    },
                    .scissor = {0U, 0U, width, height},
                };
            }
        }
        return result;
    }
    if (show_minimap) {
        float const dpi_scale = sanitizedDpi(requested_dpi_scale);
        float const constrained_size = std::min({192.0F * dpi_scale, static_cast<float>(height) * 0.35F,
            static_cast<float>(width) * 0.19F});
        float const size = std::max(128.0F * dpi_scale,
            std::floor(constrained_size / (16.0F * dpi_scale)) * 16.0F * dpi_scale);
        float const x = std::floor(static_cast<float>(width) - size - 34.0F * dpi_scale);
        float const y = 44.0F * dpi_scale;
        result.minimap_draw = {
            .image = 1U,
            .source = {0.0F, 0.0F, 1.0F, 1.0F},
            .destination = {x, y, size, size},
        };
        result.minimap_bounds = {x, y, size, size};
        result.show_minimap = true;
    }
    return result;
}

S2MapOverlayLayout buildS2WorldMapOverlay(
    S2WorldMapState const& state,
    S2MapImage const* const source_image,
    shared::TerrainWorld const* const world,
    double const player_x,
    double const player_y,
    uint32_t const width,
    uint32_t const height,
    float const requested_dpi_scale,
    TextRenderer& output
)
{
    output.clear();
    S2MapOverlayLayout layout;
    if (width == 0U || height == 0U) {
        return layout;
    }
    float const dpi_scale = sanitizedDpi(requested_dpi_scale);
    bool const source_matches = source_image
        && (!world || source_image->source_identity == world->identity());
    S2MapImage const* const image = source_matches ? source_image : nullptr;
    auto const atlas = image && world ? world->atlas() : nullptr;
    bool const sample_source_matches = atlas && image
        && image->source_identity == world->identity()
        && (atlas->configuration.resolution == 256U || atlas->configuration.resolution == 512U);
    std::optional<size_t> const sample_index = sample_source_matches
        ? mapSampleIndex(world, image, player_x, player_y)
        : std::nullopt;
    uint32_t heat_count = 0U;
    for (S2WorldMapLayer const layer : {
        S2WorldMapLayer::Temperature, S2WorldMapLayer::Moisture, S2WorldMapLayer::Precipitation,
    }) {
        size_t const index = static_cast<size_t>(layer);
        heat_count += image && image->available_layers[index] && image->has_numeric_range[index]
            && state.layers[index];
    }
    auto add_panel = [&](float const x, float const y, float const panel_width, float const panel_height,
                         uint8_t const palette_index) {
        if (layout.panel_count >= layout.panels.size() || panel_width <= 0.0F || panel_height <= 0.0F) {
            return;
        }
        layout.panels[layout.panel_count++] = {
            .bounds = {x * dpi_scale, y * dpi_scale, panel_width * dpi_scale, panel_height * dpi_scale},
            .palette_index = palette_index,
        };
    };
    auto add_heat_strip = [&](std::array<float, 4U> const bounds, S2WorldMapLayer const layer,
                              std::array<float, 2U> const range) {
        if (layout.heat_strip_count < layout.heat_strips.size()) {
            layout.heat_strips[layout.heat_strip_count++] = {.bounds = bounds, .layer = layer, .range = range};
        }
    };
    auto add_color_chip = [&](float const x, float const y, Color const color) {
        if (layout.color_chip_count >= layout.color_chips.size()) {
            return;
        }
        layout.color_chips[layout.color_chip_count++] = {
            .bounds = {x * dpi_scale, y * dpi_scale, 8.0F * dpi_scale, 8.0F * dpi_scale},
            .rgba = {
                static_cast<uint8_t>(std::clamp(std::lround(color[0]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[1]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[2]), 0L, 255L)),
                255U,
            },
        };
    };
    auto outlined = [&](std::string_view const value, Color const color, float const x, float const y,
                        float const text_scale = 1.0F) {
        submitMapText(output, value, color, x, y, dpi_scale, true, 0.5F * text_scale);
    };
    auto plain = [&](std::string_view const value, Color const color, float const x, float const y,
                     float const text_scale = 1.0F) {
        submitMapText(output, value, color, x, y, dpi_scale, false, 0.5F * text_scale);
    };

    float const header_height = heat_count > 0U ? 70.0F : 46.0F;
    add_panel(6.0F, 6.0F, static_cast<float>(width) / dpi_scale - 12.0F, header_height, 1U);
    add_panel(6.0F, static_cast<float>(height) / dpi_scale - 26.0F,
        static_cast<float>(width) / dpi_scale - 12.0F, 20.0F, 1U);

    if (!image) {
        outlined("Map source unavailable", {213.0F, 201.0F, 172.0F}, 12.0F,
            static_cast<float>(height) / dpi_scale * 0.5F);
    }

    std::string view = state.overview
        ? "Whole world"
        : fixedText(static_cast<float>(std::clamp(state.zoom, 1.0, 6.0)), 2) + "x / Player view";
    std::string header = "MAP N^ / " + view;
    if (heat_count > 0U) {
        header += " / Opaque field";
        if (heat_count > 1U) {
            header += " average";
        }
    }
    outlined(header, {240.0F, 231.0F, 204.0F}, 12.0F, 8.0F);
    std::string const help = "0 Whole world  R Reset  L "
        + std::string(state.legend_expanded ? "Hide details" : "Details");
    outlined(help, {212.0F, 221.0F, 208.0F},
        static_cast<float>(width) / dpi_scale - static_cast<float>(help.size()) * 8.0F - 12.0F, 8.0F);

    static constexpr std::array<std::string_view, S2_WORLD_MAP_LAYER_COUNT> KEYS{
        "P", "H", "T", "B", "W", "C", "O", "Q",
    };
    static constexpr std::array<std::string_view, S2_WORLD_MAP_LAYER_COUNT> SHORT_NAMES{
        "Plate", "Height", "Temp", "Biomes", "Wind", "Curr", "Moist", "Rain",
    };
    static constexpr std::array<std::string_view, S2_WORLD_MAP_LAYER_COUNT> FULL_NAMES{
        "PLATES", "HEIGHTS", "TEMPERATURE", "BIOMES", "WIND", "CURRENTS", "MOISTURE", "PRECIPITATION",
    };
    static constexpr std::array<Color, S2_WORLD_MAP_LAYER_COUNT> LAYER_COLORS{{
        {213.0F, 164.0F, 237.0F}, {217.0F, 206.0F, 170.0F}, {239.0F, 183.0F, 123.0F},
        {168.0F, 200.0F, 147.0F}, {99.0F, 219.0F, 212.0F}, {220.0F, 238.0F, 247.0F},
        {121.0F, 194.0F, 168.0F}, {201.0F, 168.0F, 230.0F},
    }};
    float cursor = 12.0F;
    for (size_t index = 0U; index < S2_WORLD_MAP_LAYER_COUNT; ++index) {
        bool const available = image && image->available_layers[index];
        std::string const status = available ? (state.layers[index] ? "ON" : "OFF") : "N/A";
        std::string const label = std::string(KEYS[index]) + " " + std::string(SHORT_NAMES[index]) + " " + status;
        outlined(label, available && state.layers[index] ? LAYER_COLORS[index] : Color{147.0F, 167.0F, 170.0F},
            cursor, 32.0F);
        cursor += static_cast<float>((SHORT_NAMES[index].size() + 6U) * 8U + 12U);
    }

    float const column_width = heat_count > 0U
        ? (static_cast<float>(width) / dpi_scale - 24.0F) / static_cast<float>(heat_count)
        : 0.0F;
    uint32_t heat_column = 0U;
    for (S2WorldMapLayer const layer : {
        S2WorldMapLayer::Temperature, S2WorldMapLayer::Moisture, S2WorldMapLayer::Precipitation,
    }) {
        size_t const index = static_cast<size_t>(layer);
        if (!image || !image->available_layers[index] || !image->has_numeric_range[index]
            || !state.layers[index]) {
            continue;
        }
        std::string const label = std::string(KEYS[index]) + " " + rangeText(*image, layer)
            + (layer == S2WorldMapLayer::Temperature ? " C" : " model");
        float const x = 12.0F + static_cast<float>(heat_column) * column_width;
        outlined(label, {231.0F, 234.0F, 219.0F},
            x, 56.0F);
        float const bar_x = x + static_cast<float>(label.size()) * 8.0F + 8.0F;
        add_heat_strip({bar_x * dpi_scale, 60.0F * dpi_scale, 48.0F * dpi_scale, 8.0F * dpi_scale},
            layer, image->layer_ranges[index]);
        if (layer == S2WorldMapLayer::Precipitation) {
            outlined("log", {212.0F, 197.0F, 230.0F}, bar_x + 54.0F, 56.0F);
        }
        ++heat_column;
    }

    if (state.legend_expanded) {
        float const x = 10.0F;
        float const y = heat_count > 0U ? 84.0F : 60.0F;
        float const overlay_width = std::min(static_cast<float>(width) / dpi_scale - 20.0F, 1'040.0F);
        float const details_column_width = (overlay_width - 30.0F) * 0.5F;
        add_panel(x, y, overlay_width, 232.0F, 2U);
        std::string source = image && sample_source_matches
            ? " / SEED " + std::to_string(world->identity().seed) + " / "
                + std::to_string(atlas->configuration.resolution) + "x" + std::to_string(atlas->configuration.resolution)
            : " / SOURCE UNAVAILABLE";
        plain("LAYER DETAILS / L Close" + source, {231.0F, 226.0F, 204.0F}, x + 10.0F, y + 8.0F);
        bool const ice_available = image && image->has_display_ice_mask;
        std::string const ice_rule = ice_available
            ? "ICE PROXY: Snow land / water <= -10 C"
            : "ICE PROXY: source unavailable";
        plain(ice_rule, {181.0F, 216.0F, 220.0F},
            x + overlay_width - static_cast<float>(ice_rule.size()) * 8.0F - 10.0F, y + 8.0F);

        auto const sample_float = [&](std::vector<float> const* const values) -> std::optional<float> {
            if (!values || !sample_index || *sample_index >= values->size()
                || !std::isfinite((*values)[*sample_index])) {
                return std::nullopt;
            }
            return (*values)[*sample_index];
        };
        auto const layer_detail = [&](size_t const index) {
            std::array<std::string, 2U> info{"", "Source unavailable; no values supplied"};
            if (!image || !image->available_layers[index]) {
                return info;
            }
            info[1].clear();
            if (index == static_cast<size_t>(S2WorldMapLayer::Plates)) {
                if (atlas && sample_index && *sample_index < atlas->plate_id.size()) {
                    info[0] = "ID " + std::to_string(atlas->plate_id[*sample_index]);
                }
                info[1] = "Edges; model world/epoch drift arrows";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Heights)) {
                if (auto const value = sample_float(atlas ? &atlas->elevation : nullptr)) {
                    info[0] = numberText(*value) + " km";
                }
                float const step = niceStep((image->layer_ranges[index][1] - image->layer_ranges[index][0]) / 8.0F);
                info[1] = numberText(step) + " km contours / " + rangeText(*image, S2WorldMapLayer::Heights)
                    + " km / sea 0";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Temperature)) {
                if (auto const value = sample_float(atlas ? &atlas->temperature : nullptr)) {
                    info[0] = numberText(*value) + " C";
                }
                info[1] = "Opaque field / " + rangeText(*image, S2WorldMapLayer::Temperature) + " C";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Biomes)) {
                if (atlas && sample_index && *sample_index < atlas->climate_zone.size()) {
                    static constexpr std::array<std::string_view, 6U> NAMES{
                        "Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow",
                    };
                    uint8_t const zone = atlas->climate_zone[*sample_index];
                    info[0] = zone < NAMES.size() ? std::string(NAMES[zone]) : "Zone " + std::to_string(zone);
                }
                info[1] = "Coarse climate-zone proxy";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Wind)) {
                info[1] = "Cyan single arrows; cells/climate step";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Currents)) {
                info[1] = "White double arrows; cells/climate step";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Moisture)) {
                if (auto const value = sample_float(atlas ? &atlas->moisture : nullptr)) {
                    info[0] = numberText(*value) + " model";
                }
                info[1] = "Reservoir " + rangeText(*image, S2WorldMapLayer::Moisture) + " / linear color";
            } else {
                if (auto const value = sample_float(atlas ? &atlas->precipitation : nullptr)) {
                    info[0] = numberText(*value) + " model";
                }
                info[1] = "Accum rain " + rangeText(*image, S2WorldMapLayer::Precipitation) + " / log color";
            }
            return info;
        };

        for (size_t index = 0U; index < S2_WORLD_MAP_LAYER_COUNT; ++index) {
            size_t const column = index / 4U;
            size_t const row = index % 4U;
            float const row_x = x + 10.0F + static_cast<float>(column) * (details_column_width + 10.0F);
            float const row_y = y + 32.0F + static_cast<float>(row) * 38.0F;
            bool const available = image && image->available_layers[index];
            std::string const status = available ? (state.layers[index] ? "ON" : "OFF") : "N/A";
            std::array<std::string, 2U> const info = layer_detail(index);
            std::string const title = std::string(KEYS[index]) + " " + std::string(FULL_NAMES[index]) + " " + status
                + (info[0].empty() ? "" : " / " + info[0]);
            plain(fitMapText(title, details_column_width, dpi_scale),
                available && state.layers[index] ? LAYER_COLORS[index] : Color{187.0F, 197.0F, 190.0F},
                row_x, row_y);
            plain(fitMapText(info[1], details_column_width, dpi_scale), {151.0F, 173.0F, 177.0F}, row_x, row_y + 18.0F);
        }

        std::string note = image && image->has_display_ice_mask
            ? "Ice proxy: " + std::to_string(image->ice_snow_count) + " land / "
                + std::to_string(image->ice_water_count) + " water. Climate view, not freezing physics. Arrows: direction only."
            : "Ice proxy unavailable. Arrows show direction, not speed.";
        plain(fitMapText(std::move(note), overlay_width - 20.0F, dpi_scale),
            {170.0F, 189.0F, 183.0F}, x + 10.0F, y + 186.0F);
        if (image && image->available_layers[static_cast<size_t>(S2WorldMapLayer::Biomes)]) {
            plain("B:", {158.0F, 177.0F, 179.0F}, x + 10.0F, y + 208.0F);
            static constexpr std::array<std::string_view, 6U> NAMES{
                "Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow",
            };
            float biome_cursor = x + 38.0F;
            static constexpr std::array<Color, 6U> BIOME_COLORS{{
                {35.0F, 69.0F, 87.0F}, {181.0F, 153.0F, 99.0F}, {76.0F, 125.0F, 91.0F},
                {119.0F, 148.0F, 107.0F}, {153.0F, 172.0F, 154.0F}, {222.0F, 231.0F, 224.0F},
            }};
            for (uint8_t zone = 0U; zone < NAMES.size(); ++zone) {
                add_color_chip(biome_cursor, y + 212.0F, BIOME_COLORS[zone]);
                plain(NAMES[zone], {185.0F, 198.0F, 187.0F}, biome_cursor + 12.0F, y + 208.0F);
                biome_cursor += 24.0F + static_cast<float>(NAMES[zone].size()) * 8.0F;
            }
        }
        if (image && image->available_layers[static_cast<size_t>(S2WorldMapLayer::Heights)]) {
            float height_cursor = x + 498.0F;
            plain("H km:", {158.0F, 177.0F, 179.0F}, height_cursor, y + 208.0F);
            height_cursor += 48.0F;
            Range const height_range{
                image->layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)][0],
                image->layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)][1],
            };
            for (float const value : {height_range.minimum, 0.0F, height_range.maximum}) {
                std::string const label = numberText(value, 1);
                add_color_chip(height_cursor, y + 212.0F, elevationColor(value, height_range));
                plain(label, elevationColor(value, height_range), height_cursor + 12.0F, y + 208.0F);
                height_cursor += 24.0F + static_cast<float>(label.size()) * 8.0F;
            }
        }
    }

    outlined("Arrows pan  Shift faster  +/- zoom  M/Esc Close",
        {215.0F, 223.0F, 205.0F}, 12.0F, static_cast<float>(height) / dpi_scale - 24.0F);

    if (image && std::isfinite(player_x) && std::isfinite(player_y)) {
        S2MapLayout const map_layout = layoutS2Map(state, width, height, true, false, dpi_scale);
        float const player_u = static_cast<float>(wrapWorld(player_x) / WORLD_SIZE);
        float const player_v = static_cast<float>(wrapWorld(player_y) / WORLD_SIZE);
        for (uint32_t draw_index = 0U; draw_index < map_layout.world_draw_count; ++draw_index) {
            MapImageDraw const& draw = map_layout.world_draws[draw_index];
            if (player_u < draw.source[0] || player_u > draw.source[0] + draw.source[2]
                || player_v < draw.source[1] || player_v > draw.source[1] + draw.source[3]) {
                continue;
            }
            float const player_screen_x = draw.destination[0]
                + (player_u - draw.source[0]) / draw.source[2] * draw.destination[2];
            float const player_screen_y = draw.destination[1]
                + (player_v - draw.source[1]) / draw.source[3] * draw.destination[3];
            float const player_logical_x = player_screen_x / dpi_scale;
            float const player_logical_y = player_screen_y / dpi_scale;
            outlined("+", {255.0F, 228.0F, 154.0F}, player_logical_x - 8.0F, player_logical_y - 16.0F, 2.0F);
            outlined("YOU", {255.0F, 228.0F, 154.0F}, player_logical_x + 12.0F, player_logical_y + 8.0F);
        }
    }
    return layout;
}

std::optional<S2MapImage> composeS2WorldMapView(
    S2MapImage const* const world_image,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t const width,
    uint32_t const height
)
{
    if (width == 0U || height == 0U || overlay_layout.panel_count > overlay_layout.panels.size()
        || overlay_layout.heat_strip_count > overlay_layout.heat_strips.size()
        || overlay_layout.color_chip_count > overlay_layout.color_chips.size()) {
        return std::nullopt;
    }
    if (world_image && (world_image->width == 0U || world_image->height == 0U
        || world_image->rgba.size() != static_cast<uint64_t>(world_image->width) * world_image->height)) {
        return std::nullopt;
    }
    float const scale = std::min(1.0F, 512.0F / static_cast<float>(std::max(width, height)));
    uint32_t const output_width = std::max(1U, static_cast<uint32_t>(std::lround(width * scale)));
    uint32_t const output_height = std::max(1U, static_cast<uint32_t>(std::lround(height * scale)));
    S2MapImage view{
        .width = output_width,
        .height = output_height,
        .source_identity = world_image ? world_image->source_identity : shared::TerrainIdentity{},
        .source_model_identity = world_image ? world_image->source_model_identity : shared::TERRAIN_LOD_MODEL,
    };
    view.rgba.resize(static_cast<size_t>(output_width) * output_height);
    Color const canvas{
        static_cast<float>(S2_WORLD_MAP_OVERLAY_PALETTE[0U][0U]),
        static_cast<float>(S2_WORLD_MAP_OVERLAY_PALETTE[0U][1U]),
        static_cast<float>(S2_WORLD_MAP_OVERLAY_PALETTE[0U][2U]),
    };
    auto const drawAt = [&](float const screen_x, float const screen_y, size_t& source_index) {
        if (!world_image) {
            return false;
        }
        uint32_t const draw_count = std::min<uint32_t>(map_layout.world_draw_count,
            static_cast<uint32_t>(map_layout.world_draws.size()));
        for (uint32_t draw_index = 0U; draw_index < draw_count; ++draw_index) {
            MapImageDraw const& draw = map_layout.world_draws[draw_index];
            if (!validMapImageDraw(draw) || draw.image != 0U) {
                continue;
            }
            float const x = draw.destination[0];
            float const y = draw.destination[1];
            float const draw_width = draw.destination[2];
            float const draw_height = draw.destination[3];
            if (screen_x < x || screen_y < y || screen_x >= x + draw_width || screen_y >= y + draw_height) {
                continue;
            }
            if (draw.scissor[2] != 0U
                && (screen_x < draw.scissor[0] || screen_y < draw.scissor[1]
                    || screen_x >= draw.scissor[0] + draw.scissor[2]
                    || screen_y >= draw.scissor[1] + draw.scissor[3])) {
                continue;
            }
            float const u = draw.source[0] + (screen_x - x) / draw_width * draw.source[2];
            float const v = draw.source[1] + (screen_y - y) / draw_height * draw.source[3];
            uint32_t const source_x = std::min(static_cast<uint32_t>(u * world_image->width), world_image->width - 1U);
            uint32_t const source_y = std::min(static_cast<uint32_t>(v * world_image->height), world_image->height - 1U);
            source_index = static_cast<size_t>(source_y) * world_image->width + source_x;
            return true;
        }
        return false;
    };
    auto blendPanel = [](Color& base, S2MapOverlayLayout::Panel const& panel) {
        if (panel.palette_index >= S2_WORLD_MAP_OVERLAY_PALETTE.size()) {
            return;
        }
        auto const& rgba = S2_WORLD_MAP_OVERLAY_PALETTE[panel.palette_index];
        float const alpha = static_cast<float>(rgba[3U]) / 255.0F;
        for (size_t channel = 0U; channel < base.size(); ++channel) {
            base[channel] = base[channel] * (1.0F - alpha) + static_cast<float>(rgba[channel]) * alpha;
        }
    };
    for (uint32_t y = 0U; y < output_height; ++y) {
        float const screen_y = (static_cast<float>(y) + 0.5F) * static_cast<float>(height) / output_height;
        for (uint32_t x = 0U; x < output_width; ++x) {
            float const screen_x = (static_cast<float>(x) + 0.5F) * static_cast<float>(width) / output_width;
            Color color = canvas;
            size_t source_index = 0U;
            if (drawAt(screen_x, screen_y, source_index)) {
                auto const& pixel = world_image->rgba[source_index];
                color = {static_cast<float>(pixel[0U]), static_cast<float>(pixel[1U]), static_cast<float>(pixel[2U])};
            }
            for (uint32_t panel_index = 0U; panel_index < overlay_layout.panel_count; ++panel_index) {
                S2MapOverlayLayout::Panel const& panel = overlay_layout.panels[panel_index];
                auto const& bounds = panel.bounds;
                if (screen_x >= bounds[0U] && screen_y >= bounds[1U]
                    && screen_x < bounds[0U] + bounds[2U] && screen_y < bounds[1U] + bounds[3U]) {
                    blendPanel(color, panel);
                }
            }
            for (uint32_t strip_index = 0U; strip_index < overlay_layout.heat_strip_count; ++strip_index) {
                S2MapOverlayLayout::HeatStrip const& strip = overlay_layout.heat_strips[strip_index];
                auto const& bounds = strip.bounds;
                if (screen_x < bounds[0U] || screen_y < bounds[1U]
                    || screen_x >= bounds[0U] + bounds[2U] || screen_y >= bounds[1U] + bounds[3U]
                    || bounds[2U] <= 0.0F) {
                    continue;
                }
                float const position = std::clamp((screen_x - bounds[0U]) / bounds[2U], 0.0F, 1.0F);
                float const range_span = strip.range[1U] - strip.range[0U];
                float value = strip.range[0U] + range_span * position;
                if (strip.layer == S2WorldMapLayer::Precipitation) {
                    value = strip.range[0U] + std::expm1(std::log1p(range_span) * position);
                }
                color = heatColor(strip.layer, value, {strip.range[0U], strip.range[1U]});
            }
            for (uint32_t chip_index = 0U; chip_index < overlay_layout.color_chip_count; ++chip_index) {
                S2MapOverlayLayout::ColorChip const& chip = overlay_layout.color_chips[chip_index];
                auto const& bounds = chip.bounds;
                if (screen_x >= bounds[0U] && screen_y >= bounds[1U]
                    && screen_x < bounds[0U] + bounds[2U] && screen_y < bounds[1U] + bounds[3U]) {
                    color = {static_cast<float>(chip.rgba[0U]), static_cast<float>(chip.rgba[1U]),
                        static_cast<float>(chip.rgba[2U])};
                }
            }
            view.rgba[static_cast<size_t>(y) * output_width + x] = {
                static_cast<uint8_t>(std::clamp(std::lround(color[0U]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[1U]), 0L, 255L)),
                static_cast<uint8_t>(std::clamp(std::lround(color[2U]), 0L, 255L)),
                255U,
            };
        }
    }
    return view;
}

bool applyS2WorldMapAction(
    S2WorldMapState& state,
    S2WorldMapAction const action,
    double const player_x,
    double const player_y,
    bool const fast_pan
) noexcept
{
    switch (action) {
    case S2WorldMapAction::TogglePlates:
    case S2WorldMapAction::ToggleHeights:
    case S2WorldMapAction::ToggleTemperature:
    case S2WorldMapAction::ToggleBiomes:
    case S2WorldMapAction::ToggleWind:
    case S2WorldMapAction::ToggleCurrents:
    case S2WorldMapAction::ToggleMoisture:
    case S2WorldMapAction::TogglePrecipitation: {
        size_t const index = static_cast<size_t>(action);
        state.layers[index] = !state.layers[index];
        return true;
    }
    case S2WorldMapAction::PanLeft:
    case S2WorldMapAction::PanRight:
    case S2WorldMapAction::PanUp:
    case S2WorldMapAction::PanDown: {
        double const step = (fast_pan ? 0.018 : 0.006) / std::clamp(state.zoom, 1.0, 6.0) * WORLD_SIZE;
        if (action == S2WorldMapAction::PanLeft) state.center_x -= step;
        if (action == S2WorldMapAction::PanRight) state.center_x += step;
        if (action == S2WorldMapAction::PanUp) state.center_y -= step;
        if (action == S2WorldMapAction::PanDown) state.center_y += step;
        state.center_x = wrapWorld(state.center_x);
        state.center_y = wrapWorld(state.center_y);
        state.overview = false;
        return true;
    }
    case S2WorldMapAction::ZoomIn:
        state.zoom = std::min(6.0, std::max(1.0, state.zoom) + 0.25);
        state.overview = false;
        return true;
    case S2WorldMapAction::ZoomOut:
        state.zoom = std::max(1.0, std::min(6.0, state.zoom) - 0.25);
        state.overview = false;
        return true;
    case S2WorldMapAction::Overview:
        state.overview = true;
        state.zoom = 1.0;
        state.center_x = WORLD_SIZE * 0.5;
        state.center_y = WORLD_SIZE * 0.5;
        return true;
    case S2WorldMapAction::Reset:
        state.overview = false;
        state.zoom = 1.0;
        state.center_x = wrapWorld(player_x);
        state.center_y = wrapWorld(player_y);
        return true;
    case S2WorldMapAction::ToggleLegend:
        state.legend_expanded = !state.legend_expanded;
        return true;
    }
    return false;
}

} // namespace client
