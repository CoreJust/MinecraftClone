#include <client/render/CanonicalTerrainField.hpp>

#include <shared/world/Chunk.hpp>
#include <generation/V9Provider.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

namespace client::render {
namespace {

constexpr uint32_t V9_SOURCE_RESOLUTION = 256U;
constexpr size_t V9_SOURCE_COUNT = static_cast<size_t>(V9_SOURCE_RESOLUTION) * V9_SOURCE_RESOLUTION;
constexpr double V9_COAST_SAND_MAX_ELEVATION_M = 20.0;
constexpr uint8_t V9_COLD_SUMMIT_BIOME = 0U;
constexpr uint8_t V9_HOT_DESERT_BIOME = 5U;

template<typename T>
[[nodiscard]] std::span<T const> requireV9GridField(
    std::span<generation::V9RegisteredFieldView const> fields,
    std::string_view const name,
    generation::V9FieldType const type,
    std::vector<uint32_t> const& shape,
    size_t const expected_size
)
{
    auto const field = std::find_if(fields.begin(), fields.end(), [name](auto const& candidate) {
        return candidate.name == name;
    });
    if (field == fields.end() || field->type != type || field->shape != shape) {
        throw std::invalid_argument("V9 canonical terrain field has an unexpected type or shape");
    }
    auto const* const values = std::get_if<std::span<T const>>(&field->values);
    if (values == nullptr || values->size() != expected_size) {
        throw std::invalid_argument("V9 canonical terrain field has an unexpected value count");
    }
    return *values;
}

[[nodiscard]] bool adjacentOceanOrLake(
    std::span<uint8_t const> ocean,
    std::span<uint8_t const> lake,
    uint32_t const x,
    uint32_t const y
) noexcept
{
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            uint32_t const neighbor_x = static_cast<uint32_t>(
                (static_cast<int32_t>(x) + dx + static_cast<int32_t>(V9_SOURCE_RESOLUTION))
                % static_cast<int32_t>(V9_SOURCE_RESOLUTION)
            );
            uint32_t const neighbor_y = static_cast<uint32_t>(
                (static_cast<int32_t>(y) + dy + static_cast<int32_t>(V9_SOURCE_RESOLUTION))
                % static_cast<int32_t>(V9_SOURCE_RESOLUTION)
            );
            size_t const neighbor = static_cast<size_t>(neighbor_y) * V9_SOURCE_RESOLUTION + neighbor_x;
            if (ocean[neighbor] != 0U || lake[neighbor] != 0U) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<CanonicalTerrainFieldValue> buildV9CanonicalTerrainFieldValues(
    generation::GlobalAtlas const& atlas
)
{
    if (atlas.configuration.model_tag != generation::TerrainModelTag::V9) {
        throw std::invalid_argument("canonical V9 terrain fields require a V9 atlas");
    }
    std::vector<generation::V9RegisteredFieldView> const fields = generation::v9RegisteredFields(atlas);
    std::span<double const> const elevation = requireV9GridField<double>(
        fields, "elevation", generation::V9FieldType::Float64,
        {V9_SOURCE_RESOLUTION, V9_SOURCE_RESOLUTION}, V9_SOURCE_COUNT
    );
    std::span<float const> const temperature = requireV9GridField<float>(
        fields, "temperature_C", generation::V9FieldType::Float32,
        {8U, V9_SOURCE_RESOLUTION, V9_SOURCE_RESOLUTION}, V9_SOURCE_COUNT * 8U
    );
    std::span<uint8_t const> const biome = requireV9GridField<uint8_t>(
        fields, "biome_id", generation::V9FieldType::UInt8,
        {V9_SOURCE_RESOLUTION, V9_SOURCE_RESOLUTION}, V9_SOURCE_COUNT
    );
    std::span<uint8_t const> const ocean = requireV9GridField<uint8_t>(
        fields, "ocean", generation::V9FieldType::Bool,
        {V9_SOURCE_RESOLUTION, V9_SOURCE_RESOLUTION}, V9_SOURCE_COUNT
    );
    std::span<uint8_t const> const lake = requireV9GridField<uint8_t>(
        fields, "lake", generation::V9FieldType::Bool,
        {V9_SOURCE_RESOLUTION, V9_SOURCE_RESOLUTION}, V9_SOURCE_COUNT
    );
    std::span<uint8_t const> const river = requireV9GridField<uint8_t>(
        fields, "river", generation::V9FieldType::Bool,
        {V9_SOURCE_RESOLUTION, V9_SOURCE_RESOLUTION}, V9_SOURCE_COUNT
    );

    std::vector<CanonicalTerrainFieldValue> result(V9_SOURCE_COUNT);
    for (uint32_t y = 0U; y < V9_SOURCE_RESOLUTION; ++y) {
        for (uint32_t x = 0U; x < V9_SOURCE_RESOLUTION; ++x) {
            size_t const index = static_cast<size_t>(y) * V9_SOURCE_RESOLUTION + x;
            double phase_mean_temperature = 0.0;
            for (size_t phase = 0U; phase < 8U; ++phase) {
                phase_mean_temperature += temperature[phase * V9_SOURCE_COUNT + index];
            }
            phase_mean_temperature /= 8.0;
            double const source_elevation = elevation[index];
            if (!std::isfinite(phase_mean_temperature) || !std::isfinite(source_elevation)) {
                throw std::invalid_argument("V9 canonical terrain fields contain nonfinite values");
            }

            bool const water_present = ocean[index] != 0U || lake[index] != 0U || river[index] != 0U;
            bool const submerged = generation::mapV9ElevationToBlock(source_elevation) < atlas.block_sea_level;
            bool const exposed_dry_land = !submerged && !water_present;
            bool const low_coast = adjacentOceanOrLake(ocean, lake, x, y)
                && source_elevation > 0.0 && source_elevation <= V9_COAST_SAND_MAX_ELEVATION_M;
            shared::Block top_material = shared::Block::Grass;
            if (submerged) {
                top_material = shared::Block::Dirt;
            } else if (exposed_dry_land
                && (biome[index] == V9_HOT_DESERT_BIOME || low_coast)) {
                top_material = shared::Block::Sand;
            } else if (exposed_dry_land && biome[index] == V9_COLD_SUMMIT_BIOME
                && phase_mean_temperature < 0.0) {
                top_material = shared::Block::Snow;
            }

            result[index] = {
                static_cast<float>(phase_mean_temperature),
                static_cast<float>(source_elevation),
                static_cast<float>(static_cast<uint8_t>(top_material)),
                0.0F,
            };
        }
    }
    return result;
}

} // namespace client::render
