#include "V9WaterClimate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace generation::detail
{

namespace
{

template <typename Result>
void classifyBiomes(Result& result, V9WaterLandClimate const& climate, std::span<double const> snow_history,
                    std::span<double const> glacier_history, std::span<uint8_t const> ocean,
                    std::span<uint8_t const> lake, std::span<uint8_t const> river = {})
{
    size_t const size = ocean.size();
    uint32_t const phases = climate.phase_count;
    result.aridity_index.resize(size);
    result.biome_id.resize(size);
    for (size_t i = 0; i < size; ++i) {
        double mean = 0, precipitation = 0, warm = -std::numeric_limits<double>::infinity();
        double cold = std::numeric_limits<double>::infinity(), driest = cold, stock = cold;
        for (uint32_t phase = 0; phase < phases; ++phase) {
            size_t const offset = size * phase + i;
            double const temperature = climate.temperature_C[offset], rain = climate.precipitation_mm_year[offset];
            mean += temperature;
            precipitation += rain;
            warm = std::max(warm, temperature);
            cold = std::min(cold, temperature);
            driest = std::min(driest, rain);
            stock = std::min(stock, snow_history[offset] + .917 * glacier_history[offset]);
        }
        mean /= phases;
        precipitation /= phases;
        double const aridity = precipitation / std::max(100.0, 58.93 * std::max(mean, 0.0));
        uint8_t biome = 4;
        if (warm < 10) {
            biome = 1;
        }
        if (warm >= 10 && cold < -3) {
            biome = 2;
        }
        if (aridity < .65 && mean < 18) {
            biome = 3;
        }
        if (aridity < .2 && mean < 18) {
            biome = 9;
        }
        if (mean >= 18 && aridity < .2) {
            biome = 5;
        }
        if (mean >= 18 && aridity >= .2 && aridity < .65) {
            biome = 6;
        }
        if (mean >= 18 && aridity >= .65) {
            biome = driest < 720 ? 7 : 8;
        }
        if (warm <= 0) {
            biome = 9;
        }
        if (stock > .1) {
            biome = 0;
        }
        if (ocean[i]) {
            biome = 10;
        }
        if (lake[i]) {
            biome = 11;
        }
        if (!river.empty() && river[i] && !(ocean[i] || lake[i])) {
            biome = 12;
        }
        result.aridity_index[i] = static_cast<float>(aridity);
        result.biome_id[i] = biome;
    }
}

} // namespace

std::expected<V9WaterLandInventory, V9WaterFailure> deriveV9WaterLandInventory(V9WaterLandClimate const& climate,
                            std::span<uint8_t const> ocean,
                            std::span<uint8_t const> lake)
{
    size_t const size = ocean.size();
    uint32_t const phases = climate.phase_count;
    if ((phases != 4 && phases != 8) || size == 0 || lake.size() != size ||
        climate.temperature_C.size() != size * phases || climate.precipitation_mm_year.size() != size * phases ||
        std::any_of(climate.temperature_C.begin(), climate.temperature_C.end(),
                    [](double v) { return !std::isfinite(v); }) ||
        std::any_of(climate.precipitation_mm_year.begin(), climate.precipitation_mm_year.end(),
                    [](double v) { return !std::isfinite(v) || v < 0; })) {
        return std::unexpected(
            V9WaterFailure{V9WaterFailure::Kind::InvalidInput,
                           "four or eight matching finite seasonal fields and nonnegative precipitation required"});
    }
    std::vector<double> snow(size), ice(size), snow_history(size * phases), glacier_history(size * phases);
    V9WaterLandInventory result;
    for (uint32_t year = 0; year < 20; ++year) {
        for (uint32_t phase = 0; phase < phases; ++phase) {
            double added = 0, melted = 0;
            for (size_t i = 0; i < size; ++i) {
                size_t const offset = size * phase + i;
                double const temperature = climate.temperature_C[offset];
                double const addition = temperature <= 0 && !(ocean[i] || lake[i])
                            ? climate.precipitation_mm_year[offset] / 1'000 / phases
                            : 0;
                snow[i] += addition;
                added += addition;
                double const capacity = std::max(temperature, 0.0) * 360 / phases * .003;
                double const snow_melt = std::min(snow[i], capacity);
                snow[i] -= snow_melt;
                double const ice_melt = std::min(ice[i], std::max(capacity - snow_melt, 0.0) * (.008 / .003));
                ice[i] -= ice_melt;
                melted += snow_melt + ice_melt;
                if (year == 19) {
                    snow_history[offset] = snow[i];
                    glacier_history[offset] = ice[i] / .917;
                }
            }
            result.accumulation_grid_sum_m += added;
            result.melt_grid_sum_m += melted;
        }
        for (size_t i = 0; i < size; ++i) {
            ice[i] += snow[i];
            snow[i] = 0;
        }
    }
    result.remaining_grid_sum_m = std::accumulate(ice.begin(), ice.end(), 0.0);
    result.snow_water_equivalent_m.assign(snow_history.begin(), snow_history.end());
    result.glacier_thickness_m.assign(glacier_history.begin(), glacier_history.end());
    classifyBiomes(result, climate, snow_history, glacier_history, ocean, lake);
    return result;
}

std::expected<V9WaterBiomeClassification, V9WaterFailure>
classifyV9WaterLocalBiomes(V9WaterLandClimate const& climate, std::span<double const> snow_water_equivalent_m,
                           std::span<double const> glacier_thickness_m, std::span<uint8_t const> ocean,
                           std::span<uint8_t const> lake, std::span<uint8_t const> river)
{
    size_t const size = ocean.size(), count = size * climate.phase_count;
    if (!size || (climate.phase_count != 4 && climate.phase_count != 8) || lake.size() != size ||
        river.size() != size || climate.temperature_C.size() != count ||
        climate.precipitation_mm_year.size() != count || snow_water_equivalent_m.size() != count ||
        glacier_thickness_m.size() != count) {
        return std::unexpected(
            V9WaterFailure{V9WaterFailure::Kind::InvalidInput, "matching seasonal biome fields required"});
    }
    std::vector<double> snow(snow_water_equivalent_m.begin(), snow_water_equivalent_m.end());
    std::vector<double> glacier(glacier_thickness_m.begin(), glacier_thickness_m.end());
    for (uint32_t phase = 0; phase < climate.phase_count; ++phase) {
        for (size_t i = 0; i < size; ++i) {
            size_t const k = size_t(phase) * size + i;
            if (ocean[i] || lake[i] || river[i]) {
                snow[k] = glacier[k] = 0;
            }
            if (!std::isfinite(climate.temperature_C[k]) || !std::isfinite(climate.precipitation_mm_year[k]) ||
                climate.precipitation_mm_year[k] < 0 || !std::isfinite(snow[k]) || snow[k] < 0 ||
                !std::isfinite(glacier[k]) || glacier[k] < 0) {
                return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput,
                            "finite nonnegative biome inventories required"});
            }
        }
    }
    V9WaterBiomeClassification result;
    result.snow_water_equivalent_m.assign(snow.begin(), snow.end());
    result.glacier_thickness_m.assign(glacier.begin(), glacier.end());
    classifyBiomes(result, climate, snow, glacier, ocean, lake, river);
    return result;
}

std::expected<std::vector<double>, V9WaterFailure> registerV9WaterContinuous(std::span<double const> cells,
                            uint32_t source_resolution,
                            uint32_t target_resolution,
                            uint32_t phase_count)
{
    if (!source_resolution || !target_resolution || source_resolution > 4'096 || target_resolution > 4'096 ||
        !phase_count || phase_count > 8 ||
        cells.size() != size_t(source_resolution) * source_resolution * phase_count ||
        std::any_of(cells.begin(), cells.end(), [](double v) { return !std::isfinite(v); })) {
        return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput,
                            "matching finite phase-major cell-centre field required"});
    }
    std::vector<double> vertices(size_t(target_resolution) * target_resolution * phase_count);
    auto wrap = [source_resolution](int32_t i) {
        return static_cast<uint32_t>((i + static_cast<int32_t>(source_resolution)) %
                            static_cast<int32_t>(source_resolution));
    };
    for (uint32_t phase = 0; phase < phase_count; ++phase) {
        for (uint32_t y = 0; y < target_resolution; ++y) {
            double const sy = double(y) * source_resolution / target_resolution - .5;
            int32_t const iy = static_cast<int32_t>(std::floor(sy));
            double const fy = sy - iy;
            for (uint32_t x = 0; x < target_resolution; ++x) {
                double const sx = double(x) * source_resolution / target_resolution - .5;
                int32_t const ix = static_cast<int32_t>(std::floor(sx));
                double const fx = sx - ix;
                auto at = [&](int32_t yy, int32_t xx) {
                    return cells[(size_t(phase) * source_resolution + wrap(yy)) * source_resolution + wrap(xx)];
                };
                vertices[(size_t(phase) * target_resolution + y) * target_resolution + x] =
                    (1 - fy) * ((1 - fx) * at(iy, ix) + fx * at(iy, ix + 1)) +
                    fy * ((1 - fx) * at(iy + 1, ix) + fx * at(iy + 1, ix + 1));
            }
        }
    }
    return vertices;
}

} // namespace generation::detail
