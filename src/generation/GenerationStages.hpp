#pragma once

#include "Numerics.hpp"

#include <generation/Generation.hpp>

#include <span>
#include <stop_token>

namespace generation::detail
{

using Int16Field = std::vector<int16_t>;
using Int32Field = std::vector<int32_t>;

struct TectonicsFields
{
    FloatField elevation;
    FloatField initial_elevation;
    FloatField continental;
    FloatField uplift;
    FloatField island_arc_uplift;
    FloatField hotspot;
    FloatField subduction;
    FloatField divergence;
    FloatField plate_vx;
    FloatField plate_vy;
    Int16Field plate_id;
};

struct CoastalFields
{
    FloatField elevation;
    FloatField displacement;
    FloatField roughness_potential;
};

struct HydrologyFields
{
    FloatField elevation;
    FloatField filled;
    FloatField water_level;
    FloatField flow_accumulation;
    Int32Field downstream;
    Int32Field lake_id;
    Int32Field outlet_id;
    ByteField lake_mask;
    ByteField river_mask;
    ByteField ocean_mask;
    ByteField fjord_candidate_mask;
    ByteField harbour_candidate_mask;
    uint32_t sea_components = 0U;
    float river_threshold_flux = 0.0F;
};

struct ClimateFields
{
    FloatField temperature;
    FloatField precipitation;
    FloatField moisture;
    FloatField wind_u;
    FloatField wind_v;
    FloatField current_u;
    FloatField current_v;
    ByteField climate_zone;
    ByteField snow_mask;
};

struct HydrologyParameters final
{
    uint32_t erosion_steps;
    float sea_level;
    GenerationStage stage;
};

struct ClimateParameters final
{
    uint32_t iterations;
    float sea_level;
};

[[nodiscard]]
TectonicsFields generateTectonics(
    GenerationConfiguration const& configuration,
    std::stop_token const& cancellation,
    ProgressCallback const& progress
);

[[nodiscard]]
CoastalFields elaborateCoasts(
    GenerationConfiguration const& configuration,
    std::span<float const> elevation,
    std::span<float const> activity,
    std::stop_token const& cancellation,
    ProgressCallback const& progress
);

[[nodiscard]]
HydrologyFields buildHydrology(
    GenerationConfiguration const& configuration,
    std::span<float const> elevation,
    std::span<float const> precipitation,
    std::span<float const> temperature,
    HydrologyParameters const& parameters,
    std::stop_token const& cancellation,
    ProgressCallback const& progress
);

[[nodiscard]]
ClimateFields simulateClimate(
    GenerationConfiguration const& configuration,
    std::span<float const> elevation,
    ClimateParameters const& parameters,
    std::span<uint8_t const> ocean_mask,
    std::span<uint8_t const> lake_mask,
    GenerationStage stage,
    std::stop_token const& cancellation,
    ProgressCallback const& progress
);

} // namespace generation::detail
