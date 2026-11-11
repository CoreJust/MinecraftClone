#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace generation::detail
{

inline constexpr size_t V10_CLIMATE_PHASE_COUNT = 8U;

// Phase-major row-major doubles. Climate fields use 32-square cell centres;
// ground is static. Rain uses 256-square vertices, annualized per 360-day year.
struct V10ClimateFields final
{
    std::vector<double> local_effective_celsius;
    std::vector<double> ground_m;
    std::vector<double> theta_celsius;
    std::vector<double> precipitation_mm_year;
};

class V10ClimateForcing final
{
public:
    explicit V10ClimateForcing(V10ClimateFields fields);

    [[nodiscard]]
    V10ClimateFields const& fields() const noexcept { return m_fields; }

private:
    V10ClimateFields const m_fields;
};

struct V10WaterPresence final
{
    bool ocean = false;
    bool lake = false;
    bool river = false;
};

struct V10ClimateSample final
{
    std::array<double, V10_CLIMATE_PHASE_COUNT> temperature_celsius{};
    std::array<double, V10_CLIMATE_PHASE_COUNT> precipitation_mm_year{};
    std::array<double, V10_CLIMATE_PHASE_COUNT> snow_water_equivalent_m{};
    std::array<double, V10_CLIMATE_PHASE_COUNT> glacier_thickness_m{};
    double mean_temperature_celsius = 0.0;
    double mean_precipitation_mm_year = 0.0;
    double mean_snow_water_equivalent_m = 0.0;
    double mean_glacier_thickness_m = 0.0;
    double aridity_index = 0.0;
    uint8_t biome_id = 0U;
    V10WaterPresence water;
};

// fine-surface-climate-v1: actual bed above sea zero, with one metre per block.
// Recomputes the local 20-year snow/firn history, never interpolated biome IDs.
[[nodiscard]]
V10ClimateSample queryV10Climate(
    V10ClimateForcing const& forcing, double x_m, double y_m, double actual_bed_m,
    V10WaterPresence water = {}
);

} // namespace generation::detail
