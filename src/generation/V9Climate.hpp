#pragma once
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
namespace generation::detail
{
// Every climate state array in the frozen source is NumPy float64.
// Keep binary64 at the climate-to-water handoff; water owns its later casts.
struct V9ClimateField final
{
    std::vector<uint32_t> shape;
    std::vector<double> values;
};
struct V9ClimateResult final
{
    // Source spelling, axes, binary64 precision, and source units are preserved.
    std::map<std::string, V9ClimateField> fields;
    double thermal_residual_W_m2 = 0.;
    double dry_mass_residual_kg_m2_s = 0.;
    double water_budget_grid_sum_max_kg_m2 = 0.;
    double minimum_reservoir_kg_m2 = 0.;
    double local_phase_budget_max_kg_m2_s = 0.;
    double max_supersaturation_ratio = 0.;
    double max_cfl = 0.;
    uint64_t moisture_substeps = 0;
};
enum class V9ClimateFailure : uint8_t
{
    Cancelled,
    InvalidInput,
    NumericalFailure
};
struct V9ClimateProgress final
{
    enum class Stage : uint8_t
    {
        Thermal,
        Weather,
        Moisture,
        Complete
    };
    Stage stage;
    uint32_t completed;
    uint32_t total;
    std::string_view detail = {};
};
using V9ClimateProgressCallback = std::function<void(V9ClimateProgress)>;
// Exact finite 256² canonical terrain vertices in metres. Fixed 32² climate,
// eight thermal phases, 180 actual daily weather samples, three 360-day years.
[[nodiscard]]
std::expected<V9ClimateResult, V9ClimateFailure> generateV9Climate(
    std::span<const double> bed_vertices_m, uint64_t seed, std::stop_token stop = {},
    V9ClimateProgressCallback progress = {});
} // namespace generation::detail
