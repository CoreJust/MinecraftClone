#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace generation::detail
{
struct V9MarineFailure final
{
    enum class Kind : uint8_t
    {
        InvalidInput,
        ChemistryDomain,
        IterationLimit,
        Residual
    };
    Kind kind;
    std::string message;
};
struct V9MarineSaltInput final
{
    uint32_t resolution = 0;
    uint32_t phase_count = 0;
    uint32_t runoff_resolution = 0;
    double horizontal_metric_scale = 250.0;
    std::span<double const> wet_fraction;
    std::span<double const> east_volume_m3_s;
    // Either phase-major m*m, or phase-major (m+1)*m with first row omitted.
    std::span<double const> north_volume_m3_s;
    std::span<double const> rain_kg_m2_s;
    std::span<double const> evaporation_kg_m2_s;
    std::span<double const> runoff_outlets_m_s;
};
struct V9MarineSalt final
{
    std::vector<double> salinity_g_kg;
    double initial_salt_kg = 0;
    double final_salt_kg = 0;
    double vertical_salt_import_kg = 0;
    double vertical_salt_export_kg = 0;
    double salt_residual_kg = 0;
    double water_rate_residual_m3_s = 0;
    double max_linear_relative_residual = 0;
};
struct V9MarinePhase final
{
    double liquid_kg_m2;
    double ice_kg_m2;
    double temperature_C;
    double salinity_g_kg;
    double freezing_temperature_C;
    double enthalpy_residual_J_m2;
    uint32_t iterations;
};
[[nodiscard]] std::expected<double, V9MarineFailure> v9MarineFreezing(double salinity_g_kg);
[[nodiscard]] std::expected<V9MarinePhase, V9MarineFailure>
v9MarinePhaseEquilibrium(double total_water_kg_m2, double salt_kg_m2, double enthalpy_J_m2, bool wet);
// Matches the source's global saline-cell convergence/stopping iteration.
[[nodiscard]] std::expected<std::vector<V9MarinePhase>, V9MarineFailure>
v9MarinePhaseEquilibrium(std::span<double const> total_water_kg_m2, std::span<double const> salt_kg_m2,
                         std::span<double const> enthalpy_J_m2, std::span<uint8_t const> wet);
[[nodiscard]] std::expected<V9MarineSalt, V9MarineFailure> deriveV9MarineSalt(V9MarineSaltInput const& input);
} // namespace generation::detail
