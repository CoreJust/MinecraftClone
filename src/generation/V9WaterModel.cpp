#include "V9WaterModel.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <utility>

namespace generation::detail
{
namespace
{

constexpr uint32_t NATIVE_RESOLUTION = 256;
constexpr size_t NATIVE_SIZE = size_t(NATIVE_RESOLUTION) * NATIVE_RESOLUTION;
constexpr double MODEL_YEAR_S = 360.0 * 86'400;

std::expected<V9WaterResult, V9WaterFailure> fail(V9WaterFailure::Kind kind, std::string message)
{
    return std::unexpected(V9WaterFailure{kind, std::move(message)});
}

} // namespace

std::expected<V9WaterResult, V9WaterFailure> generateV9Water(std::span<double const> bed, V9WaterForcing const& forcing,
                            std::stop_token stop_token,
                            std::function<void(V9WaterProgress)> progress)
{
    uint32_t const m = forcing.resolution, phases = forcing.phase_count;
    size_t const coarse_size = size_t(m) * m, phase_size = coarse_size * phases;
    if (!m || NATIVE_RESOLUTION % m || (phases != 4 && phases != 8) || bed.size() != NATIVE_SIZE ||
        !std::isfinite(forcing.horizontal_metric_scale) || forcing.horizontal_metric_scale <= 0) {
        return fail(V9WaterFailure::Kind::InvalidInput,
                    "finite 256-square bed and four/eight matching climate phases required");
    }
    for (auto const* field : {&forcing.ground_m, &forcing.reference_ps, &forcing.wet_fraction}) {
        if (field->size() != coarse_size ||
            std::any_of(field->begin(), field->end(), [](double v) { return !std::isfinite(v); })) {
            return fail(V9WaterFailure::Kind::InvalidInput, "matching finite static climate fields required");
        }
    }
    for (auto const* field :
         {&forcing.local_effective_C, &forcing.rain_kg_m2_s, &forcing.runoff_kg_m2_s, &forcing.u_m_s, &forcing.v_m_s,
          &forcing.layer0_vapor_RH, &forcing.ocean_east_volume_m3_s, &forcing.ocean_evap_kg_m2_s}) {
        if (field->size() != phase_size ||
            std::any_of(field->begin(), field->end(), [](double v) { return !std::isfinite(v); })) {
            return fail(V9WaterFailure::Kind::InvalidInput, "matching finite seasonal climate fields required");
        }
    }
    if ((!forcing.theta_C.empty() &&
         (forcing.theta_C.size() != phase_size ||
          std::any_of(forcing.theta_C.begin(), forcing.theta_C.end(), [](double v) { return !std::isfinite(v); }))) ||
        (forcing.ocean_north_volume_m3_s.size() != phase_size &&
         forcing.ocean_north_volume_m3_s.size() != size_t(phases) * (m + 1) * m)) {
        return fail(V9WaterFailure::Kind::InvalidInput,
                    "matching ocean temperature and registered north face fields required");
    }
    if (stop_token.stop_requested()) {
        return fail(V9WaterFailure::Kind::Cancelled, "water generation cancelled");
    }
    auto report = [&](V9WaterPhase phase, uint32_t completed = 0, uint32_t total = 0) {
        if (progress) {
            progress({phase, completed, total});
        }
    };
    report(V9WaterPhase::Routing);
    auto registered = [&](std::span<double const> field, uint32_t count) {
        return *registerV9WaterContinuous(field, m, NATIVE_RESOLUTION, count);
    };
    auto temperature = registered(forcing.local_effective_C, phases);
    auto rain = registered(forcing.rain_kg_m2_s, phases);
    auto runoff = registered(forcing.runoff_kg_m2_s, phases);
    auto const ground = registered(forcing.ground_m, 1);
    std::vector<double> evaporation(phase_size);
    for (uint32_t phase = 0; phase < phases; ++phase) {
        for (size_t i = 0; i < coarse_size; ++i) {
            size_t const k = size_t(phase) * coarse_size + i;
            double const t = forcing.local_effective_C[k];
            double const es = 611.2 * std::exp(17.67 * t / (t + 243.5));
            double const qs = .622 * es / (forcing.reference_ps[i] - .378 * es);
            evaporation[k] = .0015 * std::hypot(forcing.u_m_s[k], forcing.v_m_s[k]) * qs *
                            std::max(1 - forcing.layer0_vapor_RH[k], 0.0);
        }
    }
    if (std::any_of(evaporation.begin(), evaporation.end(), [](double v) { return !std::isfinite(v) || v < 0; })) {
        return fail(V9WaterFailure::Kind::InvalidInput, "invalid supplied atmospheric evaporation state");
    }
    evaporation = registered(evaporation, phases);
    V9WaterRates rates{std::vector<double>(NATIVE_SIZE), std::vector<double>(NATIVE_SIZE),
                       std::vector<double>(NATIVE_SIZE)};
    for (uint32_t phase = 0; phase < phases; ++phase) {
        for (size_t i = 0; i < NATIVE_SIZE; ++i) {
            size_t const k = size_t(phase) * NATIVE_SIZE + i;
            temperature[k] -= .0065 * (std::max(bed[i], 0.0) - ground[i]);
            if (rain[k] < 0 || runoff[k] < 0) {
                return fail(V9WaterFailure::Kind::InvalidInput, "negative supplied freshwater forcing");
            }
            rates.evaporation_m_s[i] += evaporation[k];
            rates.precipitation_m_s[i] += rain[k];
            rates.runoff_m_s[i] += runoff[k];
            rain[k] *= MODEL_YEAR_S;
            runoff[k] *= MODEL_YEAR_S;
        }
    }
    for (size_t i = 0; i < NATIVE_SIZE; ++i) {
        rates.evaporation_m_s[i] = rates.evaporation_m_s[i] / phases / 1'000;
        rates.precipitation_m_s[i] = rates.precipitation_m_s[i] / phases / 1'000;
        rates.runoff_m_s[i] = rates.runoff_m_s[i] / phases / 1'000;
    }
    auto water = solveV9WaterEquilibrium(bed, NATIVE_RESOLUTION, rates);
    if (!water) {
        return std::unexpected(water.error());
    }
    if (stop_token.stop_requested()) {
        return fail(V9WaterFailure::Kind::Cancelled, "water generation cancelled after equilibrium");
    }
    auto routing = routeV9Water(*water);
    if (!routing) {
        return std::unexpected(routing.error());
    }
    report(V9WaterPhase::MarineSalt);
    std::vector<double> runoff_outlets(NATIVE_SIZE);
    for (size_t i = 0; i < NATIVE_SIZE; ++i) {
        runoff_outlets[i] = water->ocean[i] ? routing->discharge_m3_s[i] / (256.0 * 256) : 0;
    }
    auto salt = deriveV9MarineSalt({
        .resolution = m,
        .phase_count = phases,
        .runoff_resolution = NATIVE_RESOLUTION,
        .horizontal_metric_scale = forcing.horizontal_metric_scale,
        .wet_fraction = forcing.wet_fraction,
        .east_volume_m3_s = forcing.ocean_east_volume_m3_s,
        .north_volume_m3_s = forcing.ocean_north_volume_m3_s,
        .rain_kg_m2_s = forcing.rain_kg_m2_s,
        .evaporation_kg_m2_s = forcing.ocean_evap_kg_m2_s,
        .runoff_outlets_m_s = runoff_outlets,
    });
    if (!salt) {
        return fail(V9WaterFailure::Kind::MarineFailure, salt.error().message);
    }
    std::vector<double> support(coarse_size);
    for (uint32_t phase = 0; phase < phases; ++phase) {
        for (size_t i = 0; i < coarse_size; ++i) {
            if (salt->salinity_g_kg[size_t(phase) * coarse_size + i] > 0) {
                support[i] = 1;
            }
        }
    }
    support = registered(support, 1);
    auto salinity = registered(salt->salinity_g_kg, phases);
    std::vector<double> freezing(salinity.size());
    for (uint32_t phase = 0; phase < phases; ++phase) {
        for (size_t i = 0; i < NATIVE_SIZE; ++i) {
            size_t const k = size_t(phase) * NATIVE_SIZE + i;
            salinity[k] = support[i] > 0 ? salinity[k] / support[i] * water->ocean[i] : 0;
            auto const tf = v9MarineFreezing(salinity[k]);
            if (!tf) {
                return fail(V9WaterFailure::Kind::MarineFailure, tf.error().message);
            }
            freezing[k] = *tf;
        }
    }
    if (stop_token.stop_requested()) {
        return fail(V9WaterFailure::Kind::Cancelled, "water generation cancelled after marine salt");
    }
    report(V9WaterPhase::SnowAndIce);
    auto land = deriveV9WaterLandInventory({phases, temperature, rain}, water->ocean, water->lake);
    if (!land) {
        return std::unexpected(land.error());
    }
    V9WaterResult result;
    result.phase_count = phases;
    result.water_level_m = water->water_level_m;
    result.water_depth_m.resize(NATIVE_SIZE);
    for (size_t i = 0; i < NATIVE_SIZE; ++i) {
        result.water_depth_m[i] = static_cast<float>(std::max(double(water->water_level_m[i]) - bed[i], 0.0));
    }
    result.temperature_C.assign(temperature.begin(), temperature.end());
    result.precipitation_mm_year.assign(rain.begin(), rain.end());
    result.runoff_mm_year.assign(runoff.begin(), runoff.end());
    result.salinity_g_kg.assign(salinity.begin(), salinity.end());
    result.freezing_temperature_C.assign(freezing.begin(), freezing.end());
    result.sea_ice_stefan_diagnostic_m.resize(salinity.size());
    std::vector<double> sea_ice(NATIVE_SIZE);
    for (uint32_t year = 0; year < 8; ++year) {
        for (uint32_t phase = 0; phase < phases; ++phase) {
            for (size_t i = 0; i < NATIVE_SIZE; ++i) {
                size_t const k = size_t(phase) * NATIVE_SIZE + i;
                double const growth =
                    2 * 2.1 * std::max(freezing[k] - temperature[k], 0.0) * (MODEL_YEAR_S / phases) / (917 * 334'000);
                sea_ice[i] = std::sqrt(sea_ice[i] * sea_ice[i] + growth) * water->ocean[i];
                sea_ice[i] =
                    std::max(sea_ice[i] - std::max(temperature[k] - freezing[k], 0.0) * 360 / phases * .006, 0.0) *
                    water->ocean[i];
                if (year == 7) {
                    result.sea_ice_stefan_diagnostic_m[k] = static_cast<float>(sea_ice[i]);
                }
            }
        }
        if (stop_token.stop_requested()) {
            return fail(V9WaterFailure::Kind::Cancelled, "water generation cancelled during ice integration");
        }
    }
    auto ocean_temperature = registered(forcing.theta_C.empty() ? forcing.local_effective_C : forcing.theta_C, phases);
    result.sea_ice_thickness_m.resize(salinity.size());
    result.ocean_surface_effective_temperature_C.resize(salinity.size());
    result.sea_ice_brine_salinity_g_kg.resize(salinity.size());
    std::vector<double> mass(NATIVE_SIZE, 20'000), solute(NATIVE_SIZE), enthalpy(NATIVE_SIZE);
    for (uint32_t phase = 0; phase < phases; ++phase) {
        for (size_t i = 0; i < NATIVE_SIZE; ++i) {
            size_t const k = size_t(phase) * NATIVE_SIZE + i;
            solute[i] = mass[i] * salinity[k] / (1'000 - salinity[k]);
            enthalpy[i] = 3'990 * mass[i] * ocean_temperature[k];
        }
        auto const states = v9MarinePhaseEquilibrium(mass, solute, enthalpy, water->ocean);
        if (!states) {
            return fail(V9WaterFailure::Kind::MarineFailure, states.error().message);
        }
        for (size_t i = 0; i < NATIVE_SIZE; ++i) {
            size_t const k = size_t(phase) * NATIVE_SIZE + i;
            auto const& state = (*states)[i];
            result.sea_ice_thickness_m[k] = static_cast<float>(state.ice_kg_m2 / 917 * water->ocean[i]);
            result.ocean_surface_effective_temperature_C[k] =
                static_cast<float>(ocean_temperature[k] * water->ocean[i]);
            result.sea_ice_brine_salinity_g_kg[k] = static_cast<float>(state.salinity_g_kg * water->ocean[i]);
            result.phase_enthalpy_residual_J_m2 =
                std::max(result.phase_enthalpy_residual_J_m2, std::abs(state.enthalpy_residual_J_m2));
        }
        report(V9WaterPhase::SnowAndIce, phase + 1, phases);
        if (stop_token.stop_requested()) {
            return fail(V9WaterFailure::Kind::Cancelled, "water generation cancelled during phase inversion");
        }
    }
    for (auto const& body : water->bodies) {
        result.native_body_catalogue.insert(result.native_body_catalogue.end(), {
                            double(body.id),
                            body.stage_m,
                            body.spill_m,
                            body.volume_m3,
                            body.area_m2,
                            body.source_m3_s,
                            body.precipitation_m3_s,
                            body.evaporation_m3_s,
                            body.replaced_runoff_m3_s,
                            body.overflow_m3_s,
                            });
        int32_t receiver = -1;
        for (auto const& transfer : water->spill_transfers) {
            if (transfer.source_node == body.id) {
                receiver = transfer.entry_vertex;
            }
        }
        result.native_body_receivers.insert(result.native_body_receivers.end(), {body.id, receiver});
    }
    result.equilibrium = std::move(*water);
    result.routing = std::move(*routing);
    result.land_inventory = std::move(*land);
    result.salt_budget = std::move(*salt);
    report(V9WaterPhase::Complete, phases, phases);
    return result;
}

} // namespace generation::detail
