#pragma once

#include <generation/Generation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace generation
{

enum class V9FieldType : uint8_t
{
    Bool,
    UInt8,
    UInt32,
    Int32,
    Int64,
    Float32,
    Float64,
};

enum class V9FieldRegistration : uint8_t
{
    PeriodicVertex,
    PeriodicCellCentre,
    PlateLookup,
    Native,
};

using V9FieldValues = std::variant<
    std::span<uint8_t const>,
    std::span<uint32_t const>,
    std::span<int32_t const>,
    std::span<int64_t const>,
    std::span<float const>,
    std::span<double const>
>;

struct V9RegisteredFieldView final
{
    std::string_view name;
    V9FieldType type = V9FieldType::Float64;
    V9FieldRegistration registration = V9FieldRegistration::Native;
    std::vector<uint32_t> shape;
    V9FieldValues values = std::span<double const>{};
};

struct V9PlateDirection final
{
    uint32_t plate_id = 0U;
    float x = 0.0F;
    float y = 0.0F;
};

struct V9SurfaceSample final
{
    double elevation_m = 0.0;
    double water_level_m = 0.0;
    double temperature_celsius = 0.0;
    double precipitation_mm_year = 0.0;
    uint8_t biome_id = 0U;
    bool ocean = false;
    bool lake = false;
    bool river = false;
};

struct V9WaterGrid final
{
    uint32_t rows = 0U;
    uint32_t columns = 0U;
    double origin_x_m = 0.0;
    double origin_y_m = 0.0;
    double spacing_m = 0.0;
    std::span<double const> bed_m;
};

struct V9WaterGridResult final
{
    uint32_t rows = 0U;
    uint32_t columns = 0U;
    std::vector<uint8_t> ocean;
    std::vector<uint8_t> lake;
    std::vector<uint8_t> river;
    std::vector<int32_t> lake_id;
    std::vector<float> water_level_m;
    std::vector<float> water_depth_m;
};

struct V9WaterQueryFailure final
{
    std::string message;
    bool cancelled = false;
};

[[nodiscard]]
std::vector<V9RegisteredFieldView> v9RegisteredFields(GlobalAtlas const& atlas);

[[nodiscard]]
std::span<uint32_t const> v9CurrentPlateIds(GlobalAtlas const& atlas);

[[nodiscard]]
std::span<V9PlateDirection const> v9PlateDirections(GlobalAtlas const& atlas);

[[nodiscard]]
V9SurfaceSample sampleV9Surface(GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod);

// Proves wetness for every integer block column covered by an aligned V9 source cell.
[[nodiscard]]
bool v9WaterCoversIntegerBlockCell(
    GlobalAtlas const& atlas,
    uint32_t origin_x,
    uint32_t origin_y,
    uint32_t extent
) noexcept;

[[nodiscard]]
std::expected<V9WaterGridResult, V9WaterQueryFailure> queryV9WaterGrid(
    GlobalAtlas const& atlas,
    V9WaterGrid const& grid,
    std::stop_token cancellation = {}
);

[[nodiscard]]
uint16_t mapV9ElevationToBlock(double source_elevation_m);

[[nodiscard]]
uint16_t mapV9WaterLevelToBlock(double source_water_level_m);

} // namespace generation
