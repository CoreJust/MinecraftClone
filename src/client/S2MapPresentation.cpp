#include <client/S2MapPresentation.hpp>

#include <client/GenerationMapPresentation.hpp>
#include <client/render/TextRenderer.hpp>

#include <generation/Generation.hpp>
#include <generation/V9Provider.hpp>

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
        || lod.terrain_identity != world.identity()
        || lod.model_identity != shared::terrainLodModel(world.atlas()->configuration.model_tag)
        || lod.level != shared::TerrainLodLevel::L0 || lod.extent != generation::WORLD_SIZE_BLOCKS
        || lod.origin_x != 0U || lod.origin_y != 0U
        || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return false;
    }
    uint32_t const expected_spacing = shared::terrainLodSpacing(
        atlas->configuration.resolution, shared::TerrainLodLevel::L0, atlas->configuration.model_tag
    );
    uint32_t const expected_side = generation::WORLD_SIZE_BLOCKS / expected_spacing + 1U;
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
generation::V9RegisteredFieldView const* v9Field(
    std::span<generation::V9RegisteredFieldView const> fields,
    std::string_view const name
) noexcept
{
    auto const found = std::find_if(fields.begin(), fields.end(), [name](auto const& field) {
        return field.name == name;
    });
    return found == fields.end() ? nullptr : &*found;
}

[[nodiscard]]
bool hasV9Shape(
    generation::V9RegisteredFieldView const* const field,
    std::span<uint32_t const> expected_shape
) noexcept
{
    if (field == nullptr || !std::ranges::equal(field->shape, expected_shape)) {
        return false;
    }
    size_t expected_count = 1U;
    for (uint32_t const extent : expected_shape) {
        if (extent == 0U || expected_count > std::numeric_limits<size_t>::max() / extent) {
            return false;
        }
        expected_count *= extent;
    }
    return std::visit([expected_count](auto const values) {
        return values.size() == expected_count;
    }, field->values);
}

[[nodiscard]]
double v9ValueAt(generation::V9RegisteredFieldView const& field, size_t const index) noexcept
{
    return std::visit([index](auto const values) {
        return index < values.size()
            ? static_cast<double>(values[index])
            : std::numeric_limits<double>::quiet_NaN();
    }, field.values);
}

[[nodiscard]]
bool v9FiniteField(generation::V9RegisteredFieldView const& field) noexcept
{
    return std::visit([](auto const values) {
        return std::all_of(values.begin(), values.end(), [](auto const value) {
            return std::isfinite(static_cast<double>(value));
        });
    }, field.values);
}

[[nodiscard]]
bool v9BooleanField(generation::V9RegisteredFieldView const& field) noexcept
{
    return std::visit([](auto const values) {
        return std::all_of(values.begin(), values.end(), [](auto const value) {
            double const converted = static_cast<double>(value);
            return converted == 0.0 || converted == 1.0;
        });
    }, field.values);
}

[[nodiscard]]
double v9PhaseMean(
    generation::V9RegisteredFieldView const& field,
    size_t const phase_size,
    size_t const index
) noexcept
{
    double total = 0.0;
    for (size_t phase = 0U; phase < 8U; ++phase) {
        total += v9ValueAt(field, phase * phase_size + index);
    }
    return total / 8.0;
}

[[nodiscard]]
std::vector<float> v9MeanPhaseField(
    generation::V9RegisteredFieldView const& field,
    size_t const phase_size
)
{
    std::vector<float> result(phase_size);
    for (size_t index = 0U; index < phase_size; ++index) {
        result[index] = static_cast<float>(v9PhaseMean(field, phase_size, index));
    }
    return result;
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
    case shared::Block::Sand: return {229.0F, 211.0F, 162.0F};
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

[[nodiscard]]
std::vector<AxisSegment> splitAxis(float start, float extent)
{
    std::vector<AxisSegment> result;
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
        && lod.model_identity == (lod.terrain_identity.terrain_version
            == generation::terrainVersion(generation::TerrainModelTag::V9)
            ? shared::TERRAIN_LOD_MODEL_V9 : shared::TERRAIN_LOD_MODEL)
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
    uint32_t const resolution = image->source_resolution != 0U
        ? image->source_resolution : atlas->configuration.resolution;
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

using ScreenPoint = std::array<float, 2U>;
using PixelColor = std::array<uint8_t, 4U>;

struct ScreenRect final {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};

[[nodiscard]]
PixelColor pixelColor(Color const color, uint8_t const alpha = 255U) noexcept
{
    return {
        static_cast<uint8_t>(std::clamp(std::lround(color[0U]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[1U]), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(color[2U]), 0L, 255L)),
        alpha,
    };
}

[[nodiscard]]
bool intersects(ScreenRect const& left, ScreenRect const& right) noexcept
{
    return left.left < right.right && left.right > right.left
        && left.top < right.bottom && left.bottom > right.top;
}

[[nodiscard]]
bool contains(ScreenRect const& outer, ScreenRect const& inner) noexcept
{
    return inner.left >= outer.left && inner.top >= outer.top
        && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

constexpr size_t MAX_GUI_TRIANGLE_VERTICES =
    MAXIMUM_GUI_VECTOR_VERTICES - MAXIMUM_GUI_VECTOR_VERTICES % 3U;

void emitTriangle(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const a,
    ScreenPoint const b,
    ScreenPoint const c,
    PixelColor const color_a,
    PixelColor const color_b,
    PixelColor const color_c
)
{
    output.push_back({.position = a, .rgba = color_a});
    output.push_back({.position = b, .rgba = color_b});
    output.push_back({.position = c, .rgba = color_c});
}

void emitSolidQuad(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const a,
    ScreenPoint const b,
    ScreenPoint const c,
    ScreenPoint const d,
    PixelColor const color
)
{
    emitTriangle(output, a, b, c, color, color, color);
    emitTriangle(output, a, c, d, color, color, color);
}

void emitFringeQuad(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const outer_a,
    ScreenPoint const outer_b,
    ScreenPoint const inner_b,
    ScreenPoint const inner_a,
    PixelColor color
)
{
    PixelColor const transparent{color[0U], color[1U], color[2U], 0U};
    emitTriangle(output, outer_a, outer_b, inner_b, transparent, transparent, color);
    emitTriangle(output, outer_a, inner_b, inner_a, transparent, color, color);
}

[[nodiscard]]
ScreenPoint offsetPoint(ScreenPoint const point, ScreenPoint const direction, float const amount) noexcept
{
    return {point[0U] + direction[0U] * amount, point[1U] + direction[1U] * amount};
}

[[nodiscard]]
bool emitStroke(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const start,
    ScreenPoint const end,
    float const width,
    PixelColor const color
)
{
    float const dx = end[0U] - start[0U];
    float const dy = end[1U] - start[1U];
    float const length = std::hypot(dx, dy);
    if (!std::isfinite(length) || length < 1.0e-4F) {
        return false;
    }
    constexpr size_t STROKE_VERTEX_COUNT = 18U;
    if (output.size() + STROKE_VERTEX_COUNT > MAX_GUI_TRIANGLE_VERTICES) {
        return false;
    }
    ScreenPoint const normal{-dy / length, dx / length};
    float const outer_half = width * 0.5F;
    float const core_half = std::max(0.0F, outer_half - 0.55F);
    ScreenPoint const outer_left_start = offsetPoint(start, normal, outer_half);
    ScreenPoint const outer_left_end = offsetPoint(end, normal, outer_half);
    ScreenPoint const inner_left_start = offsetPoint(start, normal, core_half);
    ScreenPoint const inner_left_end = offsetPoint(end, normal, core_half);
    ScreenPoint const outer_right_start = offsetPoint(start, normal, -outer_half);
    ScreenPoint const outer_right_end = offsetPoint(end, normal, -outer_half);
    ScreenPoint const inner_right_start = offsetPoint(start, normal, -core_half);
    ScreenPoint const inner_right_end = offsetPoint(end, normal, -core_half);

    emitSolidQuad(output, inner_left_start, inner_left_end, inner_right_end, inner_right_start, color);
    emitFringeQuad(output, outer_left_start, outer_left_end, inner_left_end, inner_left_start, color);
    emitFringeQuad(output, outer_right_end, outer_right_start, inner_right_start, inner_right_end, color);
    return true;
}

[[nodiscard]]
bool appendScreenArrow(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const center,
    float const direction_x,
    float const direction_y,
    float const length,
    Color const color,
    bool const double_head,
    ScreenRect const& viewport,
    std::span<ScreenRect const> const excluded_rectangles
)
{
    float const magnitude = std::hypot(direction_x, direction_y);
    if (!std::isfinite(magnitude) || !std::isfinite(length) || length <= 0.0F) {
        return false;
    }
    PixelColor const foreground = pixelColor(color);
    PixelColor const outline = pixelColor(VECTOR_OUTLINE_COLOR);
    if (magnitude < 1.0e-10F) {
        ScreenRect const bounds{center[0U], center[1U], center[0U] + 2.0F, center[1U] + 2.0F};
        if (!contains(viewport, bounds)
            || std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(), [&](ScreenRect const& rect) {
                return intersects(bounds, rect);
            })
            || output.size() + 6U > MAX_GUI_TRIANGLE_VERTICES) {
            return false;
        }
        emitSolidQuad(output, center, {center[0U] + 2.0F, center[1U]},
            {center[0U] + 2.0F, center[1U] + 2.0F}, {center[0U], center[1U] + 2.0F}, foreground);
        return true;
    }

    ScreenPoint const direction{direction_x / magnitude, direction_y / magnitude};
    ScreenPoint const perpendicular{-direction[1U], direction[0U]};
    ScreenPoint const start = offsetPoint(center, direction, -length * 0.5F);
    ScreenPoint const end = offsetPoint(center, direction, length * 0.5F);
    std::array<ScreenPoint, 8U> bounds_points{};
    size_t bounds_point_count = 0U;
    bounds_points[bounds_point_count++] = start;
    bounds_points[bounds_point_count++] = end;
    size_t const head_count = double_head ? 2U : 1U;
    for (size_t head = 0U; head < head_count; ++head) {
        float const offset = head == 0U ? 0.0F : 4.0F;
        ScreenPoint const tip = offsetPoint(end, direction, -offset);
        bounds_points[bounds_point_count++] = tip;
        bounds_points[bounds_point_count++] = offsetPoint(
            offsetPoint(tip, direction, -4.0F), perpendicular, -3.0F);
        bounds_points[bounds_point_count++] = offsetPoint(
            offsetPoint(tip, direction, -4.0F), perpendicular, 3.0F);
    }
    float left = bounds_points[0U][0U];
    float right = left;
    float top = bounds_points[0U][1U];
    float bottom = top;
    for (size_t index = 1U; index < bounds_point_count; ++index) {
        left = std::min(left, bounds_points[index][0U]);
        right = std::max(right, bounds_points[index][0U]);
        top = std::min(top, bounds_points[index][1U]);
        bottom = std::max(bottom, bounds_points[index][1U]);
    }
    constexpr float EDGE_MARGIN = 2.25F;
    ScreenRect const bounds{left - EDGE_MARGIN, top - EDGE_MARGIN, right + EDGE_MARGIN, bottom + EDGE_MARGIN};
    if (!contains(viewport, bounds)
        || std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(), [&](ScreenRect const& rect) {
            return intersects(bounds, rect);
        })) {
        return false;
    }

    size_t const stroke_count = 3U + (double_head ? 2U : 0U);
    size_t const required_vertices = stroke_count * 2U * 18U;
    if (output.size() + required_vertices > MAX_GUI_TRIANGLE_VERTICES) {
        return false;
    }
    auto stroke = [&](ScreenPoint const from, ScreenPoint const to) {
        static_cast<void>(emitStroke(output, from, to, 3.5F, outline));
        static_cast<void>(emitStroke(output, from, to, 1.5F, foreground));
    };
    stroke(start, end);
    for (size_t head = 0U; head < head_count; ++head) {
        float const offset = head == 0U ? 0.0F : 4.0F;
        ScreenPoint const tip = offsetPoint(end, direction, -offset);
        ScreenPoint const wing_base = offsetPoint(tip, direction, -4.0F);
        stroke(tip, offsetPoint(wing_base, perpendicular, -3.0F));
        stroke(tip, offsetPoint(wing_base, perpendicular, 3.0F));
    }
    return true;
}

struct GridSampleRange final {
    uint32_t first = 0U;
    uint32_t end = 0U;
};

[[nodiscard]]
GridSampleRange mapGridSampleRange(
    float const source_start,
    float const source_extent,
    uint32_t const resolution,
    uint32_t const step,
    float const offset
) noexcept
{
    double const source_start_pixel = static_cast<double>(source_start) * resolution - 0.5;
    double const source_end_pixel = static_cast<double>(source_start + source_extent) * resolution - 0.5;
    int64_t const minimum = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(source_start_pixel)), 0, resolution);
    int64_t const maximum = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(source_end_pixel)), 0, resolution);
    uint32_t const grid_origin = static_cast<uint32_t>(std::floor(static_cast<float>(step) * offset));
    int64_t first = grid_origin;
    if (first < minimum) {
        first += ((minimum - first + step - 1U) / step) * step;
    }
    return {
        .first = static_cast<uint32_t>(std::min<int64_t>(first, resolution)),
        .end = static_cast<uint32_t>(maximum),
    };
}

template <typename TVisitor>
void forEachVisibleMapGridSample(
    S2MapLayout const& map_layout,
    uint32_t const resolution,
    uint32_t const step,
    float const offset,
    TVisitor&& visitor
)
{
    for (uint32_t draw_index = 0U; draw_index < map_layout.world_draw_count; ++draw_index) {
        MapImageDraw const& draw = map_layout.world_draws[draw_index];
        if (!validMapImageDraw(draw) || draw.image != 0U) {
            continue;
        }
        GridSampleRange const x_range = mapGridSampleRange(
            draw.source[0U], draw.source[2U], resolution, step, offset);
        GridSampleRange const y_range = mapGridSampleRange(
            draw.source[1U], draw.source[3U], resolution, step, offset);
        for (uint32_t y = y_range.first; y < y_range.end; y += step) {
            float const v = (static_cast<float>(y) + 0.5F) / static_cast<float>(resolution);
            if (v < draw.source[1U] || v >= draw.source[1U] + draw.source[3U]) {
                continue;
            }
            for (uint32_t x = x_range.first; x < x_range.end; x += step) {
                float const u = (static_cast<float>(x) + 0.5F) / static_cast<float>(resolution);
                if (u < draw.source[0U] || u >= draw.source[0U] + draw.source[2U]) {
                    continue;
                }
                ScreenPoint const screen{
                    draw.destination[0U] + (u - draw.source[0U]) / draw.source[2U] * draw.destination[2U],
                    draw.destination[1U] + (v - draw.source[1U]) / draw.source[3U] * draw.destination[3U],
                };
                ScreenRect viewport{
                    draw.destination[0U],
                    draw.destination[1U],
                    draw.destination[0U] + draw.destination[2U],
                    draw.destination[1U] + draw.destination[3U],
                };
                if (draw.scissor[2U] != 0U) {
                    viewport.left = std::max(viewport.left, static_cast<float>(draw.scissor[0U]));
                    viewport.top = std::max(viewport.top, static_cast<float>(draw.scissor[1U]));
                    viewport.right = std::min(viewport.right,
                        static_cast<float>(draw.scissor[0U] + draw.scissor[2U]));
                    viewport.bottom = std::min(viewport.bottom,
                        static_cast<float>(draw.scissor[1U] + draw.scissor[3U]));
                }
                visitor(x, y, u, v, screen, viewport);
            }
        }
    }
}

[[nodiscard]]
bool appendPointerTriangles(
    std::vector<GuiVectorVertex>& output,
    ScreenPoint const center,
    double const heading_degrees,
    float const reach,
    ScreenRect const& viewport
)
{
    constexpr size_t POINTER_VERTEX_COUNT = 78U;
    if (!std::isfinite(heading_degrees) || !std::isfinite(reach) || reach <= 0.0F
        || output.size() + POINTER_VERTEX_COUNT > MAX_GUI_TRIANGLE_VERTICES) {
        return false;
    }
    double const angle = heading_degrees * (3.141'592'653'589'793'238'46 / 180.0);
    float const cosine = static_cast<float>(std::cos(angle));
    float const sine = static_cast<float>(std::sin(angle));
    std::array<ScreenPoint, 4U> points{{
        {0.0F, -reach}, {reach * 0.65F, reach * 0.70F},
        {0.0F, reach * 0.35F}, {-reach * 0.65F, reach * 0.70F},
    }};
    for (ScreenPoint& point : points) {
        ScreenPoint const local = point;
        point = {
            center[0U] + local[0U] * cosine - local[1U] * sine,
            center[1U] + local[0U] * sine + local[1U] * cosine,
        };
    }
    ScreenRect bounds{points[0U][0U], points[0U][1U], points[0U][0U], points[0U][1U]};
    for (size_t index = 0U; index < points.size(); ++index) {
        bounds.left = std::min(bounds.left, points[index][0U] - 2.0F);
        bounds.right = std::max(bounds.right, points[index][0U] + 2.0F);
        bounds.top = std::min(bounds.top, points[index][1U] - 2.0F);
        bounds.bottom = std::max(bounds.bottom, points[index][1U] + 2.0F);
    }
    if (!contains(viewport, bounds)) {
        return false;
    }

    PixelColor const dark = pixelColor({16.0F, 33.0F, 43.0F});
    PixelColor const accent = pixelColor({255.0F, 224.0F, 128.0F});
    for (size_t index = 0U; index < points.size(); ++index) {
        size_t const next = (index + 1U) % points.size();
        if (!emitStroke(output, points[index], points[next], 4.0F, dark)) {
            return false;
        }
    }
    emitTriangle(output, points[0U], points[1U], points[2U], accent, accent, accent);
    emitTriangle(output, points[0U], points[2U], points[3U], accent, accent, accent);
    return true;
}

struct PlateAccumulator final {
    double sum_x = 0.0;
    double sum_y = 0.0;
    double center_x = 0.0;
    double center_y = 0.0;
    double nearest_distance = std::numeric_limits<double>::infinity();
    uint32_t count = 0U;
    uint32_t center_index = 0U;
};

void buildPlateMotions(
    S2MapImage& image,
    std::span<int16_t const> plate_ids,
    std::span<float const> vector_x,
    std::span<float const> vector_y,
    uint32_t const resolution
)
{
    if (plate_ids.empty() || plate_ids.size() != vector_x.size() || plate_ids.size() != vector_y.size()) {
        return;
    }
    int16_t const maximum_id = *std::max_element(plate_ids.begin(), plate_ids.end());
    if (maximum_id < 0) {
        return;
    }
    std::vector<PlateAccumulator> plates(static_cast<size_t>(maximum_id) + 1U);
    for (size_t index = 0U; index < plate_ids.size(); ++index) {
        int16_t const id = plate_ids[index];
        if (id < 0) {
            continue;
        }
        PlateAccumulator& plate = plates[static_cast<size_t>(id)];
        plate.sum_x += static_cast<double>(index % resolution);
        plate.sum_y += static_cast<double>(index / resolution);
        ++plate.count;
    }
    for (PlateAccumulator& plate : plates) {
        if (plate.count > 0U) {
            plate.center_x = plate.sum_x / static_cast<double>(plate.count);
            plate.center_y = plate.sum_y / static_cast<double>(plate.count);
        }
    }
    for (size_t index = 0U; index < plate_ids.size(); ++index) {
        int16_t const id = plate_ids[index];
        if (id < 0) {
            continue;
        }
        PlateAccumulator& plate = plates[static_cast<size_t>(id)];
        double const x = static_cast<double>(index % resolution);
        double const y = static_cast<double>(index / resolution);
        double const dx = x - plate.center_x;
        double const dy = y - plate.center_y;
        double const distance = dx * dx + dy * dy;
        if (distance < plate.nearest_distance) {
            plate.nearest_distance = distance;
            plate.center_index = static_cast<uint32_t>(index);
        }
    }
    size_t const plate_count = static_cast<size_t>(std::count_if(
        plates.begin(), plates.end(), [](PlateAccumulator const& plate) { return plate.count > 0U; }));
    image.plate_motions.reserve(plate_count);
    for (size_t id = 0U; id < plates.size(); ++id) {
        PlateAccumulator const& plate = plates[id];
        if (plate.count == 0U) {
            continue;
        }
        image.plate_motions.push_back({
            .id = static_cast<uint32_t>(id),
            .x = (static_cast<float>(plate.center_index % resolution) + 0.5F) / static_cast<float>(resolution),
            .y = (static_cast<float>(plate.center_index / resolution) + 0.5F) / static_cast<float>(resolution),
            .vector_x = vector_x[plate.center_index],
            .vector_y = vector_y[plate.center_index],
        });
    }
}

[[nodiscard]]
bool mapPointForSource(
    S2MapLayout const& map_layout,
    float const source_x,
    float const source_y,
    ScreenPoint& screen,
    ScreenRect& viewport
) noexcept
{
    uint32_t const draw_count = std::min<uint32_t>(map_layout.world_draw_count,
        static_cast<uint32_t>(map_layout.world_draws.size()));
    for (uint32_t draw_index = 0U; draw_index < draw_count; ++draw_index) {
        MapImageDraw const& draw = map_layout.world_draws[draw_index];
        if (!validMapImageDraw(draw) || draw.image != 0U
            || source_x < draw.source[0U] || source_x >= draw.source[0U] + draw.source[2U]
            || source_y < draw.source[1U] || source_y >= draw.source[1U] + draw.source[3U]) {
            continue;
        }
        screen = {
            draw.destination[0U] + (source_x - draw.source[0U]) / draw.source[2U] * draw.destination[2U],
            draw.destination[1U] + (source_y - draw.source[1U]) / draw.source[3U] * draw.destination[3U],
        };
        viewport = {
            draw.destination[0U],
            draw.destination[1U],
            draw.destination[0U] + draw.destination[2U],
            draw.destination[1U] + draw.destination[3U],
        };
        if (draw.scissor[2U] != 0U) {
            ScreenRect const scissor{
                static_cast<float>(draw.scissor[0U]),
                static_cast<float>(draw.scissor[1U]),
                static_cast<float>(draw.scissor[0U] + draw.scissor[2U]),
                static_cast<float>(draw.scissor[1U] + draw.scissor[3U]),
            };
            viewport.left = std::max(viewport.left, scissor.left);
            viewport.top = std::max(viewport.top, scissor.top);
            viewport.right = std::min(viewport.right, scissor.right);
            viewport.bottom = std::min(viewport.bottom, scissor.bottom);
        }
        return true;
    }
    return false;
}

void appendWorldMapVectors(
    S2WorldMapState const& state,
    S2MapImage const& image,
    generation::GlobalAtlas const& atlas,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t const width,
    uint32_t const height,
    float const dpi_scale,
    TextRenderer& text,
    std::vector<GuiVectorVertex>& output
)
{
    bool const v9 = atlas.configuration.model_tag == generation::TerrainModelTag::V9;
    uint32_t const resolution = v9 ? image.vector_field_resolution : atlas.configuration.resolution;
    size_t const atlas_size = static_cast<size_t>(atlas.configuration.resolution)
        * atlas.configuration.resolution;
    size_t const vector_size = static_cast<size_t>(resolution) * resolution;
    if ((atlas.configuration.resolution != 256U && atlas.configuration.resolution != 512U)
        || resolution == 0U || vector_size == 0U) {
        return;
    }
    bool const show_wind = state.layers[static_cast<size_t>(S2WorldMapLayer::Wind)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Wind)]
        && (v9 ? image.v9_wind_u_m_s.size() == vector_size && image.v9_wind_v_m_s.size() == vector_size
            : atlas.wind_u.size() == atlas_size && atlas.wind_v.size() == atlas_size);
    bool const show_currents = state.layers[static_cast<size_t>(S2WorldMapLayer::Currents)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Currents)]
        && (v9 ? image.v9_ocean_u_m_s.size() == vector_size && image.v9_ocean_v_m_s.size() == vector_size
            && image.v9_ocean_cells.size() == vector_size
            : atlas.current_u.size() == atlas_size && atlas.current_v.size() == atlas_size
                && atlas.ocean_mask.size() == atlas_size);
    bool const show_plates = state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)];
    if (!show_wind && !show_currents && !show_plates) {
        return;
    }

    std::array<ScreenRect, 15U> exclusions{};
    size_t exclusion_count = 0U;
    auto append_exclusions = [&](auto const& rectangles, uint32_t const count) {
        uint32_t const bounded_count = std::min<uint32_t>(count, static_cast<uint32_t>(rectangles.size()));
        for (uint32_t index = 0U; index < bounded_count && exclusion_count < exclusions.size(); ++index) {
            auto const& bounds = rectangles[index].bounds;
            exclusions[exclusion_count++] = {
                bounds[0U], bounds[1U], bounds[0U] + bounds[2U], bounds[1U] + bounds[3U],
            };
        }
    };
    append_exclusions(overlay_layout.panels, overlay_layout.panel_count);
    append_exclusions(overlay_layout.heat_strips, overlay_layout.heat_strip_count);
    append_exclusions(overlay_layout.color_chips, overlay_layout.color_chip_count);
    std::span<ScreenRect const> const excluded_rectangles(exclusions.data(), exclusion_count);

    float const logical_width = static_cast<float>(width) / dpi_scale;
    float const logical_height = static_cast<float>(height) / dpi_scale;
    bool const compact = logical_height < 500.0F || logical_width < 1'080.0F;
    float const tile_size = state.overview
        ? std::min(logical_width, logical_height)
        : std::max(logical_width, logical_height) * static_cast<float>(std::clamp(state.zoom, 1.0, 6.0));
    uint32_t step = std::max(3U, static_cast<uint32_t>(std::lround(
        static_cast<float>(resolution) * (compact ? 41.0F : 53.0F) / tile_size)));
    size_t const vertices_per_sample = (show_wind ? 108U : 0U) + (show_currents ? 180U : 0U);
    size_t const plate_vertex_reserve = show_plates
        ? std::min(image.plate_motions.size() * 108U, size_t{8'192U})
        : 0U;
    while (vertices_per_sample != 0U) {
        size_t const sample_axis = (resolution + step - 1U) / step;
        size_t const estimated_vertices = sample_axis * sample_axis * vertices_per_sample;
        if (estimated_vertices + plate_vertex_reserve <= MAX_GUI_TRIANGLE_VERTICES || step >= resolution) {
            break;
        }
        ++step;
    }

    if (show_wind) {
        forEachVisibleMapGridSample(map_layout, resolution, step, 0.25F,
            [&](uint32_t const x, uint32_t const y, float const, float const, ScreenPoint const screen,
                ScreenRect const& viewport) {
                size_t const index = static_cast<size_t>(y) * resolution + x;
                float const u = v9 ? image.v9_wind_u_m_s[index] : atlas.wind_u[index];
                float const v = v9 ? image.v9_wind_v_m_s[index] : atlas.wind_v[index];
                static_cast<void>(appendScreenArrow(output, screen,
                    u, v, (compact ? 13.0F : 18.0F) * dpi_scale,
                    WIND_COLOR, false, viewport, excluded_rectangles));
            });
    }
    if (show_currents) {
        forEachVisibleMapGridSample(map_layout, resolution, step, 0.70F,
            [&](uint32_t const x, uint32_t const y, float const, float const, ScreenPoint const screen,
                ScreenRect const& viewport) {
                size_t const index = static_cast<size_t>(y) * resolution + x;
                bool const ocean = v9 ? image.v9_ocean_cells[index] != 0U : atlas.ocean_mask[index] != 0U;
                if (!ocean) {
                    return;
                }
                float const u = v9 ? image.v9_ocean_u_m_s[index] : atlas.current_u[index];
                float const v = v9 ? image.v9_ocean_v_m_s[index] : atlas.current_v[index];
                static_cast<void>(appendScreenArrow(output, screen,
                    u, v, (compact ? 13.0F : 18.0F) * dpi_scale,
                    CURRENT_COLOR, true, viewport, excluded_rectangles));
            });
    }

    if (!show_plates) {
        return;
    }
    for (S2PlateMotion const& plate : image.plate_motions) {
        ScreenPoint screen{};
        ScreenRect viewport{};
        if (!mapPointForSource(map_layout, plate.x, plate.y, screen, viewport)) {
            continue;
        }
        ScreenPoint const center{screen[0U], screen[1U] - 10.0F * dpi_scale};
        float const length = (compact ? 17.0F : 24.0F) * dpi_scale;
        static_cast<void>(appendScreenArrow(output, center, plate.vector_x, plate.vector_y, length,
            PLATE_COLOR, false, viewport, excluded_rectangles));

        std::string const label = "P" + std::to_string(plate.id);
        ScreenRect const label_bounds{
            screen[0U] - 8.0F * dpi_scale,
            screen[1U] + 4.0F * dpi_scale,
            screen[0U] - 8.0F * dpi_scale + static_cast<float>(label.size()) * 8.0F * dpi_scale,
            screen[1U] + 20.0F * dpi_scale,
        };
        bool const panel_overlap = std::any_of(excluded_rectangles.begin(), excluded_rectangles.end(),
            [&](ScreenRect const& rect) { return intersects(label_bounds, rect); });
        if (contains(viewport, label_bounds) && !panel_overlap) {
            submitMapText(text, label, {235.0F, 201.0F, 248.0F}, screen[0U] / dpi_scale - 8.0F,
                screen[1U] / dpi_scale + 4.0F, dpi_scale, true);
        }
    }
}

[[nodiscard]]
std::optional<S2MapImage> rasterizeS2V9WorldMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& world_lod,
    S2WorldMapState const& state
)
{
    auto const atlas = world.atlas();
    if (!atlas || !validL0Source(world, world_lod)) {
        return std::nullopt;
    }
    constexpr uint32_t SOURCE_RESOLUTION = 256U;
    constexpr uint32_t CLIMATE_RESOLUTION = 32U;
    constexpr size_t SOURCE_COUNT = static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION;
    constexpr size_t CLIMATE_COUNT = static_cast<size_t>(CLIMATE_RESOLUTION) * CLIMATE_RESOLUTION;
    constexpr size_t V9_BIOME_COUNT = 13U;
    constexpr std::array<uint32_t, 2U> SOURCE_SHAPE{SOURCE_RESOLUTION, SOURCE_RESOLUTION};
    constexpr std::array<uint32_t, 3U> SOURCE_PHASE_SHAPE{8U, SOURCE_RESOLUTION, SOURCE_RESOLUTION};
    constexpr std::array<uint32_t, 3U> CLIMATE_PHASE_SHAPE{8U, CLIMATE_RESOLUTION, CLIMATE_RESOLUTION};

    std::vector<generation::V9RegisteredFieldView> const views = generation::v9RegisteredFields(*atlas);
    auto const* elevation = v9Field(views, "elevation");
    auto const* temperature = v9Field(views, "temperature_C");
    auto const* precipitation = v9Field(views, "precipitation_mm_year");
    auto const* biome = v9Field(views, "biome_id");
    auto const* ocean = v9Field(views, "ocean");
    auto const* lake = v9Field(views, "lake");
    auto const* river = v9Field(views, "river");
    auto const* water_level = v9Field(views, "water_level");
    auto const* wind_u = v9Field(views, "u");
    auto const* wind_v = v9Field(views, "v");
    auto const* ocean_u = v9Field(views, "ocean_u");
    auto const* ocean_v = v9Field(views, "ocean_v");
    auto const* moisture = v9Field(views, "bucket_kg_m2");
    auto const* land_snow = v9Field(views, "land_snow_water_equivalent_m");
    auto const* glacier = v9Field(views, "glacier_thickness_m");
    auto const* sea_ice = v9Field(views, "sea_ice_thickness_m");
    if (!hasV9Shape(elevation, SOURCE_SHAPE)
        || !hasV9Shape(temperature, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(precipitation, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(biome, SOURCE_SHAPE)
        || !hasV9Shape(ocean, SOURCE_SHAPE)
        || !hasV9Shape(lake, SOURCE_SHAPE)
        || !hasV9Shape(river, SOURCE_SHAPE)
        || !hasV9Shape(water_level, SOURCE_SHAPE)
        || !hasV9Shape(wind_u, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(wind_v, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(ocean_u, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(ocean_v, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(moisture, CLIMATE_PHASE_SHAPE)
        || !hasV9Shape(land_snow, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(glacier, SOURCE_PHASE_SHAPE)
        || !hasV9Shape(sea_ice, SOURCE_PHASE_SHAPE)
        || !v9FiniteField(*elevation) || !v9FiniteField(*temperature)
        || !v9FiniteField(*precipitation) || !v9FiniteField(*water_level)
        || !v9FiniteField(*biome)
        || !v9FiniteField(*wind_u) || !v9FiniteField(*wind_v)
        || !v9FiniteField(*ocean_u) || !v9FiniteField(*ocean_v)
        || !v9FiniteField(*moisture) || !v9FiniteField(*land_snow)
        || !v9FiniteField(*glacier) || !v9FiniteField(*sea_ice)
        || !v9BooleanField(*ocean) || !v9BooleanField(*lake) || !v9BooleanField(*river)) {
        return std::nullopt;
    }
    std::span<uint32_t const> const plate_ids = generation::v9CurrentPlateIds(*atlas);
    std::span<generation::V9PlateDirection const> const plate_directions = generation::v9PlateDirections(*atlas);
    if (plate_ids.size() != SOURCE_COUNT) {
        return std::nullopt;
    }

    S2MapImage image;
    image.width = S2_WORLD_MAP_IMAGE_SIDE;
    image.height = S2_WORLD_MAP_IMAGE_SIDE;
    image.source_identity = world.identity();
    image.source_model_identity = world_lod.model_identity;
    image.source_resolution = SOURCE_RESOLUTION;
    image.vector_field_resolution = CLIMATE_RESOLUTION;
    image.v9_plate_ids.assign(plate_ids.begin(), plate_ids.end());
    image.v9_elevation_m.resize(SOURCE_COUNT);
    image.v9_temperature_mean_c = v9MeanPhaseField(*temperature, SOURCE_COUNT);
    image.v9_precipitation_mean_mm_year = v9MeanPhaseField(*precipitation, SOURCE_COUNT);
    image.v9_moisture_mean_kg_m2 = v9MeanPhaseField(*moisture, CLIMATE_COUNT);
    image.v9_wind_u_m_s = v9MeanPhaseField(*wind_u, CLIMATE_COUNT);
    image.v9_wind_v_m_s = v9MeanPhaseField(*wind_v, CLIMATE_COUNT);
    image.v9_ocean_u_m_s = v9MeanPhaseField(*ocean_u, CLIMATE_COUNT);
    image.v9_ocean_v_m_s = v9MeanPhaseField(*ocean_v, CLIMATE_COUNT);
    image.v9_ocean_cells.resize(CLIMATE_COUNT);
    image.v9_biome_ids.resize(SOURCE_COUNT);
    image.v9_ice_surface_kind.resize(SOURCE_COUNT);
    image.rgba.resize(WORLD_PIXEL_COUNT);
    for (size_t index = 0U; index < SOURCE_COUNT; ++index) {
        image.v9_elevation_m[index] = v9ValueAt(*elevation, index);
        double const biome_id = v9ValueAt(*biome, index);
        if (biome_id < 0.0 || biome_id >= static_cast<double>(V9_BIOME_COUNT)
            || std::floor(biome_id) != biome_id) {
            return std::nullopt;
        }
        image.v9_biome_ids[index] = static_cast<uint8_t>(biome_id);
        bool const is_water = v9ValueAt(*ocean, index) != 0.0
            || v9ValueAt(*lake, index) != 0.0 || v9ValueAt(*river, index) != 0.0;
        bool const snow = !is_water && v9PhaseMean(*land_snow, SOURCE_COUNT, index) > 0.0;
        bool const ice = !is_water && v9PhaseMean(*glacier, SOURCE_COUNT, index) > 0.0;
        bool const sea = v9ValueAt(*ocean, index) != 0.0
            && v9PhaseMean(*sea_ice, SOURCE_COUNT, index) > 0.0;
        image.v9_ice_surface_kind[index] = sea ? 3U : (ice ? 2U : (snow ? 1U : 0U));
        image.ice_snow_count += snow;
        image.ice_glacier_count += ice;
        image.ice_water_count += sea;
    }
    for (uint32_t y = 0U; y < CLIMATE_RESOLUTION; ++y) {
        for (uint32_t x = 0U; x < CLIMATE_RESOLUTION; ++x) {
            size_t const climate_index = static_cast<size_t>(y) * CLIMATE_RESOLUTION + x;
            size_t const source_index = static_cast<size_t>(y * 8U + 4U) * SOURCE_RESOLUTION + x * 8U + 4U;
            image.v9_ocean_cells[climate_index] = static_cast<uint8_t>(v9ValueAt(*ocean, source_index) != 0.0);
        }
    }

    static constexpr std::array<Color, 13U> BIOME_COLORS{{
        {222.0F, 231.0F, 224.0F}, {153.0F, 172.0F, 154.0F}, {62.0F, 103.0F, 78.0F},
        {154.0F, 180.0F, 117.0F}, {67.0F, 119.0F, 75.0F}, {207.0F, 175.0F, 107.0F},
        {174.0F, 174.0F, 94.0F}, {115.0F, 151.0F, 83.0F}, {43.0F, 116.0F, 63.0F},
        {169.0F, 164.0F, 146.0F}, {35.0F, 69.0F, 87.0F}, {49.0F, 102.0F, 131.0F},
        {63.0F, 138.0F, 162.0F},
    }};
    bool const fields_finite = std::all_of(image.v9_elevation_m.begin(), image.v9_elevation_m.end(),
            [](double value) { return std::isfinite(value); })
        && std::all_of(image.v9_temperature_mean_c.begin(), image.v9_temperature_mean_c.end(),
            [](float value) { return std::isfinite(value); })
        && std::all_of(image.v9_precipitation_mean_mm_year.begin(), image.v9_precipitation_mean_mm_year.end(),
            [](float value) { return std::isfinite(value); })
        && std::all_of(image.v9_moisture_mean_kg_m2.begin(), image.v9_moisture_mean_kg_m2.end(),
            [](float value) { return std::isfinite(value); })
        && std::all_of(image.v9_biome_ids.begin(), image.v9_biome_ids.end(),
            [](uint8_t value) { return value < BIOME_COLORS.size(); });
    if (!fields_finite) {
        return std::nullopt;
    }

    auto const [height_minimum, height_maximum] = std::minmax_element(
        image.v9_elevation_m.begin(), image.v9_elevation_m.end()
    );
    auto const moisture_range = fieldRange(image.v9_moisture_mean_kg_m2);
    auto const precipitation_range = fieldRange(image.v9_precipitation_mean_mm_year);
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Plates)] = {0.0F,
        static_cast<float>(*std::max_element(plate_ids.begin(), plate_ids.end()))};
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Heights)] = {
        static_cast<float>(*height_minimum), static_cast<float>(*height_maximum),
    };
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Temperature)] = {-40.0F, 30.0F};
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Biomes)] = {0.0F, 12.0F};
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Moisture)] = {
        moisture_range.minimum, moisture_range.maximum,
    };
    image.layer_ranges[static_cast<size_t>(S2WorldMapLayer::Precipitation)] = {
        precipitation_range.minimum, precipitation_range.maximum,
    };
    for (S2WorldMapLayer const layer : {
        S2WorldMapLayer::Plates, S2WorldMapLayer::Heights, S2WorldMapLayer::Temperature,
        S2WorldMapLayer::Biomes, S2WorldMapLayer::Moisture, S2WorldMapLayer::Precipitation,
    }) {
        image.has_numeric_range[static_cast<size_t>(layer)] = true;
    }
    image.available_layers.fill(true);
    image.has_display_ice_mask = true;
    std::vector<uint32_t> observed_ids(plate_ids.begin(), plate_ids.end());
    std::sort(observed_ids.begin(), observed_ids.end());
    observed_ids.erase(std::unique(observed_ids.begin(), observed_ids.end()), observed_ids.end());
    image.plate_motions.reserve(observed_ids.size());
    bool directions_complete = true;
    for (uint32_t const id : observed_ids) {
        auto const direction = std::find_if(plate_directions.begin(), plate_directions.end(), [id](auto const& item) {
            return item.plate_id == id;
        });
        if (direction == plate_directions.end() || !std::isfinite(direction->x) || !std::isfinite(direction->y)) {
            directions_complete = false;
            continue;
        }
        uint64_t count = 0U;
        double sum_x = 0.0;
        double sum_y = 0.0;
        for (size_t index = 0U; index < SOURCE_COUNT; ++index) {
            if (plate_ids[index] == id) {
                ++count;
                sum_x += static_cast<double>(index % SOURCE_RESOLUTION);
                sum_y += static_cast<double>(index / SOURCE_RESOLUTION);
            }
        }
        if (count != 0U) {
            image.plate_motions.push_back({
                .id = id,
                .x = (static_cast<float>(sum_x / static_cast<double>(count)) + 0.5F) / SOURCE_RESOLUTION,
                .y = (static_cast<float>(sum_y / static_cast<double>(count)) + 0.5F) / SOURCE_RESOLUTION,
                .vector_x = direction->x,
                .vector_y = direction->y,
            });
        }
    }
    image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        = directions_complete && image.plate_motions.size() == observed_ids.size();

    Range const height_range{
        static_cast<float>(*height_minimum), static_cast<float>(*height_maximum),
    };
    float const height_extent = std::max(0.001F, height_range.maximum - height_range.minimum);
    float const contour_step = niceStep(height_extent / 8.0F);
    auto const source_index = [](uint32_t x, uint32_t y) {
        return static_cast<size_t>(y % SOURCE_RESOLUTION) * SOURCE_RESOLUTION + x % SOURCE_RESOLUTION;
    };
    auto const root_sample = [&world_lod](uint32_t x, uint32_t y) -> shared::TerrainLodSample const& {
        return world_lod.nodes[static_cast<size_t>(y) * world_lod.side + x].sample;
    };
    for (uint32_t y = 0U; y < image.height; ++y) {
        uint32_t const source_y = y * SOURCE_RESOLUTION / image.height;
        for (uint32_t x = 0U; x < image.width; ++x) {
            uint32_t const source_x = x * SOURCE_RESOLUTION / image.width;
            size_t const index = source_index(source_x, source_y);
            bool const is_water = v9ValueAt(*ocean, index) != 0.0
                || v9ValueAt(*lake, index) != 0.0 || v9ValueAt(*river, index) != 0.0;
            uint8_t const ice_kind = image.v9_ice_surface_kind[index];
            Color color = is_water ? Color{35.0F, 69.0F, 87.0F} : materialColor(root_sample(source_x, source_y).top_material);
            if (ice_kind == 1U) {
                blend(color, {236.0F, 242.0F, 235.0F}, 0.34F);
            } else if (ice_kind == 2U) {
                blend(color, {208.0F, 225.0F, 223.0F}, 0.58F);
            } else if (ice_kind == 3U) {
                blend(color, {203.0F, 227.0F, 231.0F}, 0.62F);
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Biomes)]) {
                color = BIOME_COLORS[image.v9_biome_ids[index]];
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Heights)]) {
                float const value = static_cast<float>(image.v9_elevation_m[index]);
                color = elevationColor(value, height_range);
                uint32_t const east = (source_x + 1U) % SOURCE_RESOLUTION;
                uint32_t const south = (source_y + 1U) % SOURCE_RESOLUTION;
                bool const contour = std::floor(value / contour_step)
                    != std::floor(static_cast<float>(image.v9_elevation_m[source_index(east, source_y)]) / contour_step)
                    || std::floor(value / contour_step)
                    != std::floor(static_cast<float>(image.v9_elevation_m[source_index(source_x, south)]) / contour_step);
                if (contour) {
                    blend(color, is_water ? Color{105.0F, 151.0F, 169.0F} : Color{235.0F, 222.0F, 186.0F},
                        is_water ? 0.19F : 0.29F);
                }
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Temperature)]) {
                color = heatColor(S2WorldMapLayer::Temperature, image.v9_temperature_mean_c[index], {-40.0F, 30.0F});
            }
            uint32_t const climate_x = source_x / 8U;
            uint32_t const climate_y = source_y / 8U;
            size_t const climate_index = static_cast<size_t>(climate_y) * CLIMATE_RESOLUTION + climate_x;
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Moisture)]) {
                color = heatColor(S2WorldMapLayer::Moisture,
                    image.v9_moisture_mean_kg_m2[climate_index], moisture_range);
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Precipitation)]) {
                color = heatColor(S2WorldMapLayer::Precipitation,
                    image.v9_precipitation_mean_mm_year[index], precipitation_range);
            }
            if (is_water && ice_kind != 3U) {
                double const depth_m = std::clamp(v9ValueAt(*water_level, index) - image.v9_elevation_m[index], 0.0, 600.0);
                blend(color, {20.0F, 45.0F, 60.0F}, static_cast<float>(std::min(0.40, depth_m * 0.0007)));
            }
            if (state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)]) {
                uint32_t const plate = image.v9_plate_ids[index];
                bool const boundary = plate != image.v9_plate_ids[source_index(source_x + 1U, source_y)]
                    || plate != image.v9_plate_ids[source_index(source_x, source_y + 1U)];
                if (boundary) {
                    blend(color, PLATE_COLOR, 0.95F);
                }
            }
            setPixel(image, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
        }
    }
    return image;
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
    if (atlas->configuration.model_tag == generation::TerrainModelTag::V9) {
        return rasterizeS2V9WorldMap(world, world_lod, state);
    }
    uint32_t const resolution = atlas->configuration.resolution;
    size_t const count = static_cast<size_t>(resolution) * resolution;
    S2MapImage image{
        .width = S2_WORLD_MAP_IMAGE_SIDE,
        .height = S2_WORLD_MAP_IMAGE_SIDE,
        .source_identity = world.identity(),
        .source_model_identity = world_lod.model_identity,
        .source_resolution = resolution,
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
    if (state.layers[static_cast<size_t>(S2WorldMapLayer::Plates)]
        && image.available_layers[static_cast<size_t>(S2WorldMapLayer::Plates)]) {
        buildPlateMotions(image, atlas->plate_id, atlas->plate_vx, atlas->plate_vy, resolution);
    }

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

    return image;
}

std::shared_ptr<shared::TerrainLodArtifact const> buildS2MinimapLod(
    shared::TerrainWorld const& world,
    double const player_x,
    double const player_y,
    std::stop_token const& cancellation
) try {
    auto const atlas = world.atlas();
    if (!atlas || !shared::isValidTerrainIdentity(world.identity())
        || !std::isfinite(player_x) || !std::isfinite(player_y)
        || (atlas->configuration.resolution != 256U && atlas->configuration.resolution != 512U)) {
        return {};
    }
    uint32_t const spacing = shared::terrainLodSpacing(
        atlas->configuration.resolution, shared::TerrainLodLevel::L1, atlas->configuration.model_tag);
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
    result->model_identity = shared::terrainLodModel(atlas->configuration.model_tag);
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
} catch (generation::GenerationCancelled const&) {
    return {};
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
    return {
        .x = minimap_bounds[0] + static_cast<float>(local_x / local_lod.extent) * minimap_bounds[2],
        .y = minimap_bounds[1] + static_cast<float>(local_y / local_lod.extent) * minimap_bounds[3],
        .heading_degrees = heading,
        .visible = true,
    };
}

void appendS2MinimapPointerTriangles(
    S2MinimapMarker const& marker,
    std::array<float, 4U> const& minimap_bounds,
    float const requested_dpi_scale,
    std::vector<GuiVectorVertex>& vector_triangles
)
{
    if (!marker.visible || minimap_bounds[2U] <= 0.0F || minimap_bounds[3U] <= 0.0F) {
        return;
    }
    float const dpi_scale = sanitizedDpi(requested_dpi_scale);
    float const logical_size = minimap_bounds[2U] / dpi_scale;
    float const reach = (logical_size < 160.0F ? 9.0F : 11.0F) * dpi_scale;
    ScreenRect const bounds{
        minimap_bounds[0U],
        minimap_bounds[1U],
        minimap_bounds[0U] + minimap_bounds[2U],
        minimap_bounds[1U] + minimap_bounds[3U],
    };
    static_cast<void>(appendPointerTriangles(
        vector_triangles, {marker.x, marker.y}, marker.heading_degrees, reach, bounds));
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
    TextRenderer& output,
    std::vector<GuiVectorVertex>& vector_triangles
)
{
    output.clear();
    vector_triangles.clear();
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
        "P", "H", "T", "B", "Ctrl+W", "C", "O", "Q",
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
        cursor += static_cast<float>(label.size() * 8U + 12U);
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
                + std::to_string(image->source_resolution != 0U
                    ? image->source_resolution : atlas->configuration.resolution)
                + "x" + std::to_string(image->source_resolution != 0U
                    ? image->source_resolution : atlas->configuration.resolution)
            : " / SOURCE UNAVAILABLE";
        plain("LAYER DETAILS / L Close" + source, {231.0F, 226.0F, 204.0F}, x + 10.0F, y + 8.0F);
        bool const ice_available = image && image->has_display_ice_mask;
        bool const v9 = image && image->source_identity.terrain_version
            == generation::terrainVersion(generation::TerrainModelTag::V9);
        std::string const ice_rule = ice_available
            ? (v9 ? "MODEL STOCK: land snow / glacier / sea ice"
                  : "ICE PROXY: Snow land / water <= -10 C")
            : "ICE SOURCE UNAVAILABLE";
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
                if (v9 && sample_index && *sample_index < image->v9_plate_ids.size()) {
                    info[0] = "ID " + std::to_string(image->v9_plate_ids[*sample_index]);
                } else if (atlas && sample_index && *sample_index < atlas->plate_id.size()) {
                    info[0] = "ID " + std::to_string(atlas->plate_id[*sample_index]);
                }
                info[1] = v9 ? "Current uint32 ownership; arrows use actual per-plate unit directions"
                    : "Edges; model world/epoch drift arrows";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Heights)) {
                if (v9 && sample_index && *sample_index < image->v9_elevation_m.size()) {
                    info[0] = numberText(static_cast<float>(image->v9_elevation_m[*sample_index])) + " m";
                } else if (auto const value = sample_float(atlas ? &atlas->elevation : nullptr)) {
                    info[0] = numberText(*value) + " km";
                }
                float const step = niceStep((image->layer_ranges[index][1] - image->layer_ranges[index][0]) / 8.0F);
                info[1] = numberText(step) + (v9 ? " m" : " km") + " contours / "
                    + rangeText(*image, S2WorldMapLayer::Heights) + (v9 ? " m / sea 0 m" : " km / sea 0");
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Temperature)) {
                if (v9 && sample_index && *sample_index < image->v9_temperature_mean_c.size()) {
                    info[0] = numberText(image->v9_temperature_mean_c[*sample_index]) + " C mean";
                } else if (auto const value = sample_float(atlas ? &atlas->temperature : nullptr)) {
                    info[0] = numberText(*value) + " C";
                }
                info[1] = "Opaque field / " + rangeText(*image, S2WorldMapLayer::Temperature) + " C";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Biomes)) {
                if (v9 && sample_index && *sample_index < image->v9_biome_ids.size()) {
                    static constexpr std::array<std::string_view, 13U> NAMES{
                        "Permanent snow/glacier", "Tundra", "Boreal forest", "Temperate grassland",
                        "Temperate forest", "Hot desert", "Savanna", "Seasonal tropical forest",
                        "Tropical rainforest", "Cold desert/polar barren", "Ocean", "Lake", "River reach",
                    };
                    uint8_t const biome_id = image->v9_biome_ids[*sample_index];
                    info[0] = std::to_string(biome_id) + " " + std::string(NAMES[biome_id]);
                } else if (atlas && sample_index && *sample_index < atlas->climate_zone.size()) {
                    static constexpr std::array<std::string_view, 6U> NAMES{
                        "Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow",
                    };
                    uint8_t const zone = atlas->climate_zone[*sample_index];
                    info[0] = zone < NAMES.size() ? std::string(NAMES[zone]) : "Zone " + std::to_string(zone);
                }
                info[1] = v9 ? "V9 catalogue biome_id" : "Coarse climate-zone proxy";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Wind)) {
                info[1] = v9 ? "Mean of eight actual phase wind fields; m/s"
                    : "Cyan single arrows; cells/climate step";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Currents)) {
                info[1] = v9 ? "Mean of eight ocean-current fields at ocean cells; m/s"
                    : "White double arrows; cells/climate step";
            } else if (index == static_cast<size_t>(S2WorldMapLayer::Moisture)) {
                if (v9 && sample_index && image->source_resolution != 0U) {
                    uint32_t const x_index = static_cast<uint32_t>(*sample_index % image->source_resolution);
                    uint32_t const y_index = static_cast<uint32_t>(*sample_index / image->source_resolution);
                    size_t const climate_index = static_cast<size_t>(y_index * image->vector_field_resolution
                        / image->source_resolution) * image->vector_field_resolution
                        + x_index * image->vector_field_resolution / image->source_resolution;
                    if (climate_index < image->v9_moisture_mean_kg_m2.size()) {
                        info[0] = numberText(image->v9_moisture_mean_kg_m2[climate_index]) + " kg/m2 mean";
                    }
                } else if (auto const value = sample_float(atlas ? &atlas->moisture : nullptr)) {
                    info[0] = numberText(*value) + " model";
                }
                info[1] = v9 ? "Mean soil-water reservoir / kg/m2 / linear color"
                    : "Reservoir " + rangeText(*image, S2WorldMapLayer::Moisture) + " / linear color";
            } else {
                if (v9 && sample_index && *sample_index < image->v9_precipitation_mean_mm_year.size()) {
                    info[0] = numberText(image->v9_precipitation_mean_mm_year[*sample_index]) + " mm/year mean";
                } else if (auto const value = sample_float(atlas ? &atlas->precipitation : nullptr)) {
                    info[0] = numberText(*value) + " model";
                }
                info[1] = v9 ? "Mean annual precipitation / mm/year / log color"
                    : "Accum rain " + rangeText(*image, S2WorldMapLayer::Precipitation) + " / log color";
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
            ? (v9 ? "Model stocks: land snow " + std::to_string(image->ice_snow_count)
                    + " / glacier " + std::to_string(image->ice_glacier_count)
                    + " / sea ice " + std::to_string(image->ice_water_count) + ". Arrows show direction only."
                  : "Ice proxy: " + std::to_string(image->ice_snow_count) + " land / "
                    + std::to_string(image->ice_water_count) + " water. Climate view, not freezing physics. "
                    + "Arrows: direction only.")
            : "Ice source unavailable. Arrows show direction, not speed.";
        plain(
            fitMapText(std::move(note), overlay_width - 20.0F, dpi_scale),
            {170.0F, 189.0F, 183.0F},
            x + 10.0F,
            y + 186.0F
        );
        if (image && image->available_layers[static_cast<size_t>(S2WorldMapLayer::Biomes)]) {
            plain("B:", {158.0F, 177.0F, 179.0F}, x + 10.0F, y + 208.0F);
            float biome_cursor = x + 38.0F;
            if (v9) {
                static constexpr std::array<std::string_view, 13U> NAMES{
                    "Gl", "Tu", "Bo", "Gr", "Fo", "De", "Sa", "ST", "TR", "Po", "Oc", "La", "Ri",
                };
                static constexpr std::array<Color, 13U> BIOME_COLORS{{
                    {222.0F, 231.0F, 224.0F}, {153.0F, 172.0F, 154.0F}, {62.0F, 103.0F, 78.0F},
                    {154.0F, 180.0F, 117.0F}, {67.0F, 119.0F, 75.0F}, {207.0F, 175.0F, 107.0F},
                    {174.0F, 174.0F, 94.0F}, {115.0F, 151.0F, 83.0F}, {43.0F, 116.0F, 63.0F},
                    {169.0F, 164.0F, 146.0F}, {35.0F, 69.0F, 87.0F}, {49.0F, 102.0F, 131.0F},
                    {63.0F, 138.0F, 162.0F},
                }};
                for (uint8_t biome_id = 0U; biome_id < NAMES.size(); ++biome_id) {
                    add_color_chip(biome_cursor, y + 212.0F, BIOME_COLORS[biome_id]);
                    plain(NAMES[biome_id], {185.0F, 198.0F, 187.0F}, biome_cursor + 12.0F, y + 208.0F);
                    biome_cursor += 32.0F;
                }
            } else {
                static constexpr std::array<std::string_view, 6U> NAMES{
                    "Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow",
                };
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
        }
        if (image && image->available_layers[static_cast<size_t>(S2WorldMapLayer::Heights)]) {
            float height_cursor = x + 498.0F;
            plain(v9 ? "H m:" : "H km:", {158.0F, 177.0F, 179.0F}, height_cursor, y + 208.0F);
            height_cursor += v9 ? 40.0F : 48.0F;
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

    float const logical_width = static_cast<float>(width) / dpi_scale;
    float const logical_height = static_cast<float>(height) / dpi_scale;
    if (logical_width < 720.0F) {
        outlined("WASD/arrows pan  Drag pan  Shift fast  Wheel zoom",
            {215.0F, 223.0F, 205.0F}, 12.0F, logical_height - 40.0F);
        outlined("+/- zoom  Ctrl+W Wind  M/Esc Close",
            {215.0F, 223.0F, 205.0F}, 12.0F, logical_height - 24.0F);
    } else {
        outlined("WASD/arrows pan  Drag pan  Shift faster  Wheel zoom  +/- zoom  Ctrl+W Wind  M/Esc Close",
            {215.0F, 223.0F, 205.0F}, 12.0F, logical_height - 24.0F);
    }

    S2MapLayout const map_layout = layoutS2Map(state, width, height, true, false, dpi_scale);
    if (image && std::isfinite(player_x) && std::isfinite(player_y)) {
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
    if (image && atlas && sample_source_matches) {
        appendWorldMapVectors(state, *image, *atlas, map_layout, layout,
            width, height, dpi_scale, output, vector_triangles);
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
