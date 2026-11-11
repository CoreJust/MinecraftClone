#pragma once

#include "V9Water.hpp"
#include "V9WaterClimate.hpp"
#include "V9WaterMarine.hpp"
#include "V9WaterRouting.hpp"

#include <functional>
#include <stop_token>

namespace generation::detail
{

struct V9WaterForcing final
{
    uint32_t resolution = 0;
    uint32_t phase_count = 0;
    double horizontal_metric_scale = 250.0;
    // Converts the vertical bed unit into the climate-equivalent altitude used by the lapse rate.
    double climate_equivalent_metres_per_bed_unit = 1.0;
    // Coarse static cell-centre fields, resolution squared.
    std::vector<double> ground_m;
    std::vector<double> reference_ps;
    std::vector<double> wet_fraction;
    // Coarse phase-major fields. RH is the source's layer_vapor_RH[:,0].
    std::vector<double> local_effective_C;
    std::vector<double> theta_C;
    std::vector<double> rain_kg_m2_s;
    std::vector<double> runoff_kg_m2_s;
    std::vector<double> u_m_s;
    std::vector<double> v_m_s;
    std::vector<double> layer0_vapor_RH;
    std::vector<double> ocean_east_volume_m3_s;
    std::vector<double> ocean_north_volume_m3_s;
    std::vector<double> ocean_evap_kg_m2_s;
};

enum class V9WaterPhase : uint8_t
{
    Routing,
    MarineSalt,
    SnowAndIce,
    Complete,
};

struct V9WaterProgress final
{
    V9WaterPhase phase;
    uint32_t completed_items = 0;
    uint32_t total_items = 0;
};

struct V9WaterResult final
{
    uint32_t phase_count = 0;
    V9WaterEquilibrium equilibrium;
    V9WaterRouting routing;
    V9WaterLandInventory land_inventory;
    V9MarineSalt salt_budget;
    // v73 retained native registry: [body,10] float64 and [body,2] int32.
    std::vector<double> native_body_catalogue;
    std::vector<int32_t> native_body_receivers;
    std::vector<float> water_level_m;
    std::vector<float> water_depth_m;
    std::vector<float> temperature_C;
    std::vector<float> precipitation_mm_year;
    std::vector<float> runoff_mm_year;
    std::vector<float> salinity_g_kg;
    std::vector<float> freezing_temperature_C;
    std::vector<float> sea_ice_thickness_m;
    std::vector<float> sea_ice_stefan_diagnostic_m;
    std::vector<float> ocean_surface_effective_temperature_C;
    std::vector<float> sea_ice_brine_salinity_g_kg;
    double phase_enthalpy_residual_J_m2 = 0;
};

// Exact native-array computation retained by V9/V21. Bed and water levels use
// the caller's vertical unit: V9 supplies source metres; V21 supplies relative
// game-height blocks and sets the climate-equivalent conversion to ten.
[[nodiscard]]
std::expected<V9WaterResult, V9WaterFailure>
generateV9Water(std::span<double const> bed_vertices, V9WaterForcing const& forcing,
                std::stop_token stop_token = {}, std::function<void(V9WaterProgress)> progress = {});

} // namespace generation::detail
