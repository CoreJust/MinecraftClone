#pragma once

#include "V9Water.hpp"

namespace generation::detail
{

struct V9WaterRouting final
{
    std::vector<int32_t> downstream;
    std::vector<int64_t> native_downstream;
    std::vector<double> discharge_m3_s;
    std::vector<double> flow_m3_model_year;
    // v5 publishes these named native fields after float32 conversion.
    std::vector<float> river_discharge_m3_s;
    std::vector<float> flow;
    std::vector<float> river_coverage_fraction;
    std::vector<uint8_t> river;
    std::vector<uint8_t> river_virtual_through_lake;
    std::vector<float> river_width_m;
    std::vector<float> river_depth_m;
    std::vector<float> river_stage_m;
    double ocean_export_m3_s = 0.0;
    double body_max_residual_m3_s = 0.0;
};

// Actual downhill six-neighbor reaches, with lake bodies as virtual graph
// nodes. Only equilibrium overflow is released at witnessed spill receivers.
[[nodiscard]]
std::expected<V9WaterRouting, V9WaterFailure> routeV9Water(V9WaterEquilibrium const& equilibrium);

} // namespace generation::detail
