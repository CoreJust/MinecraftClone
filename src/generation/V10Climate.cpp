#include "V10Climate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace generation::detail
{
namespace
{

constexpr size_t CLIMATE_SIDE = 32U;
constexpr size_t RAIN_SIDE = 256U;
constexpr double WORLD_M = 65'536.0;

void validateField(std::span<double const> values, size_t count, bool nonnegative)
{
    if (values.size() != count || std::ranges::any_of(values, [nonnegative](double value) {
        return !std::isfinite(value) || (nonnegative && value < 0.0);
    }))
    {
        throw std::invalid_argument("V10 climate requires matching finite fields and nonnegative precipitation");
    }
}

double wrap(double value)
{
    double const remainder = std::fmod(value, WORLD_M);
    return remainder < 0.0 ? remainder + WORLD_M : remainder;
}

double sample(std::span<double const> values, size_t side, size_t phase, double x_m, double y_m,
    double registration_offset)
{
    double const gx = wrap(x_m) * static_cast<double>(side) / WORLD_M - registration_offset;
    double const gy = wrap(y_m) * static_cast<double>(side) / WORLD_M - registration_offset;
    int32_t const ix = static_cast<int32_t>(std::floor(gx));
    int32_t const iy = static_cast<int32_t>(std::floor(gy));
    double const u = gx - ix;
    double const v = gy - iy;
    auto const at = [&](int32_t x, int32_t y) {
        auto const n = static_cast<int32_t>(side);
        size_t const xx = static_cast<size_t>((x + n) % n);
        size_t const yy = static_cast<size_t>((y + n) % n);
        return values[(phase * side + yy) * side + xx];
    };
    // Match the reference's four-term arithmetic and [y,x] registration.
    return at(ix, iy) * (1.0 - u) * (1.0 - v)
        + at(ix + 1, iy) * u * (1.0 - v)
        + at(ix, iy + 1) * (1.0 - u) * v
        + at(ix + 1, iy + 1) * u * v;
}

} // namespace

V10ClimateForcing::V10ClimateForcing(V10ClimateFields fields)
    : m_fields{std::move(fields)}
{
    validateField(m_fields.local_effective_celsius, V10_CLIMATE_PHASE_COUNT * CLIMATE_SIDE * CLIMATE_SIDE, false);
    validateField(m_fields.theta_celsius, V10_CLIMATE_PHASE_COUNT * CLIMATE_SIDE * CLIMATE_SIDE, false);
    validateField(m_fields.ground_m, CLIMATE_SIDE * CLIMATE_SIDE, false);
    validateField(m_fields.precipitation_mm_year, V10_CLIMATE_PHASE_COUNT * RAIN_SIDE * RAIN_SIDE, true);
}

V10ClimateSample queryV10Climate(
    V10ClimateForcing const& forcing, double x_m, double y_m, double actual_bed_m, V10WaterPresence water
)
{
    if (!std::isfinite(x_m) || !std::isfinite(y_m) || !std::isfinite(actual_bed_m)
        || !std::isfinite(10.0 * actual_bed_m))
    {
        throw std::invalid_argument("V10 climate requires finite XY and bed with finite lapse height");
    }
    water.lake = water.lake && !water.ocean;
    water.river = water.river && !(water.ocean || water.lake);
    V10ClimateSample result;
    result.water = water;
    auto const& fields = forcing.fields();
    double const ground = sample(fields.ground_m, CLIMATE_SIDE, 0U, x_m, y_m, .5);
    double const lapse = .0065 * (std::max(10.0 * actual_bed_m, 0.0) - ground);
    for (size_t phase = 0U; phase < V10_CLIMATE_PHASE_COUNT; ++phase)
    {
        result.temperature_celsius[phase] = water.ocean
            ? sample(fields.theta_celsius, CLIMATE_SIDE, phase, x_m, y_m, .5)
            : sample(fields.local_effective_celsius, CLIMATE_SIDE, phase, x_m, y_m, .5) - lapse;
        result.precipitation_mm_year[phase] = sample(
            fields.precipitation_mm_year, RAIN_SIDE, phase, x_m, y_m, 0.0
        );
    }
    double snow = 0.0;
    double ice = 0.0;
    for (uint32_t year = 0U; year < 20U; ++year)
    {
        for (size_t phase = 0U; phase < V10_CLIMATE_PHASE_COUNT; ++phase)
        {
            double const temperature = result.temperature_celsius[phase];
            double const addition = temperature <= 0.0 && !(water.ocean || water.lake)
                ? result.precipitation_mm_year[phase] / 1000.0 / 8.0 : 0.0;
            snow += addition;
            double const capacity = std::max(temperature, 0.0) * 360.0 / 8.0 * .003;
            double const snow_melt = std::min(snow, capacity);
            snow -= snow_melt;
            double const ice_melt = std::min(ice, std::max(capacity - snow_melt, 0.0) * (.008 / .003));
            ice -= ice_melt;
            if (year == 19U)
            {
                result.snow_water_equivalent_m[phase] = snow;
                result.glacier_thickness_m[phase] = ice / .917;
            }
        }
        ice += snow;
        snow = 0.0;
    }
    double warm = -std::numeric_limits<double>::infinity();
    double cold = std::numeric_limits<double>::infinity();
    double driest = cold;
    double stock = cold;
    for (size_t phase = 0U; phase < V10_CLIMATE_PHASE_COUNT; ++phase)
    {
        result.mean_temperature_celsius += result.temperature_celsius[phase];
        result.mean_precipitation_mm_year += result.precipitation_mm_year[phase];
        result.mean_snow_water_equivalent_m += result.snow_water_equivalent_m[phase];
        result.mean_glacier_thickness_m += result.glacier_thickness_m[phase];
        warm = std::max(warm, result.temperature_celsius[phase]);
        cold = std::min(cold, result.temperature_celsius[phase]);
        driest = std::min(driest, result.precipitation_mm_year[phase]);
        stock = std::min(stock, result.snow_water_equivalent_m[phase] + .917 * result.glacier_thickness_m[phase]);
    }
    result.mean_temperature_celsius /= 8.0;
    result.mean_precipitation_mm_year /= 8.0;
    result.mean_snow_water_equivalent_m /= 8.0;
    result.mean_glacier_thickness_m /= 8.0;
    result.aridity_index = result.mean_precipitation_mm_year
        / std::max(100.0, 58.93 * std::max(result.mean_temperature_celsius, 0.0));
    uint8_t biome = 4U;
    if (warm < 10.0)
    {
        biome = 1U;
    }
    if (warm >= 10.0 && cold < -3.0)
    {
        biome = 2U;
    }
    if (result.aridity_index < .65 && result.mean_temperature_celsius < 18.0)
    {
        biome = 3U;
    }
    if (result.aridity_index < .2 && result.mean_temperature_celsius < 18.0)
    {
        biome = 9U;
    }
    if (result.mean_temperature_celsius >= 18.0 && result.aridity_index < .2)
    {
        biome = 5U;
    }
    if (result.mean_temperature_celsius >= 18.0 && result.aridity_index >= .2 && result.aridity_index < .65)
    {
        biome = 6U;
    }
    if (result.mean_temperature_celsius >= 18.0 && result.aridity_index >= .65)
    {
        biome = driest < 720.0 ? 7U : 8U;
    }
    if (warm <= 0.0)
    {
        biome = 9U;
    }
    if (stock > .1)
    {
        biome = 0U;
    }
    if (water.ocean)
    {
        biome = 10U;
    }
    if (water.lake)
    {
        biome = 11U;
    }
    if (water.river)
    {
        biome = 12U;
    }
    result.biome_id = biome;
    return result;
}

} // namespace generation::detail
