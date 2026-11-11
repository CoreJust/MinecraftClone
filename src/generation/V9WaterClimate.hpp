#pragma once

#include "V9Water.hpp"

#include <span>
#include <vector>

namespace generation::detail
{

struct V9WaterLandClimate final
{
    uint32_t phase_count = 0;
    // Phase-major canonical vertex fields: lapsed local temperature and
    // annualized precipitation in the source's 360-day year.
    std::vector<double> temperature_C;
    std::vector<double> precipitation_mm_year;
};

struct V9WaterLandInventory final
{
    std::vector<float> snow_water_equivalent_m;
    std::vector<float> glacier_thickness_m;
    std::vector<float> aridity_index;
    std::vector<uint8_t> biome_id;
    double accumulation_grid_sum_m = 0.0;
    double melt_grid_sum_m = 0.0;
    double remaining_grid_sum_m = 0.0;
};

// Frozen stationary 20-year snow/firn history followed by custom seasonal
// biome classification. This does not diagnose glacier flow or water ice.
[[nodiscard]]
std::expected<V9WaterLandInventory, V9WaterFailure> deriveV9WaterLandInventory(V9WaterLandClimate const& climate,
                            std::span<uint8_t const> ocean,
                            std::span<uint8_t const> lake);

struct V9WaterBiomeClassification final
{
    std::vector<float> snow_water_equivalent_m;
    std::vector<float> glacier_thickness_m;
    std::vector<float> aridity_index;
    std::vector<uint8_t> biome_id;
};

// v74 local classification: supplied phases are already lapsed to the actual
// refined bed. Water clears registered snow/glacier; actual wet river reaches
// receive class 12. No local snow chronology or second lapse is invented.
[[nodiscard]]
std::expected<V9WaterBiomeClassification, V9WaterFailure>
classifyV9WaterLocalBiomes(V9WaterLandClimate const& climate, std::span<double const> snow_water_equivalent_m,
                           std::span<double const> glacier_thickness_m, std::span<uint8_t const> ocean,
                           std::span<uint8_t const> lake, std::span<uint8_t const> river);

// Periodic bilinear cell-centre-to-vertex registration used by v5. Input is
// phase-major; phase_count=1 also handles a static field. No categorical
// field may be passed through this interpolator.
[[nodiscard]]
std::expected<std::vector<double>, V9WaterFailure>
registerV9WaterContinuous(std::span<double const> cells, uint32_t source_resolution, uint32_t target_resolution,
                          uint32_t phase_count);

} // namespace generation::detail
