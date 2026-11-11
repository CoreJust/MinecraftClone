#include "V10WaterPreparation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

namespace generation::detail
{
namespace
{

std::vector<double> const& field(V9ClimateResult const& climate, char const* name,
    std::vector<uint32_t> const& shape)
{
    auto const found = climate.fields.find(name);
    size_t size = 1U;
    for (uint32_t const extent : shape)
    {
        size *= extent;
    }
    if (found == climate.fields.end() || found->second.shape != shape || found->second.values.size() != size)
    {
        throw std::invalid_argument(std::string("V10 water requires fresh climate field ") + name);
    }
    return found->second.values;
}

V9WaterForcing makeForcing(V9ClimateResult const& climate)
{
    V9WaterForcing forcing;
    forcing.resolution = 32U;
    forcing.phase_count = 8U;
    forcing.horizontal_metric_scale = 250.0;
    forcing.climate_equivalent_metres_per_bed_unit = 10.0;
    forcing.ground_m = field(climate, "ground_m", {32U, 32U});
    forcing.reference_ps = field(climate, "reference_ps", {32U, 32U});
    forcing.wet_fraction = field(climate, "wet_fraction", {32U, 32U});
    forcing.local_effective_C = field(climate, "local_effective_C", {8U, 32U, 32U});
    forcing.theta_C = field(climate, "theta_C", {8U, 32U, 32U});
    forcing.rain_kg_m2_s = field(climate, "rain_kg_m2_s", {8U, 32U, 32U});
    forcing.runoff_kg_m2_s = field(climate, "runoff_kg_m2_s", {8U, 32U, 32U});
    forcing.u_m_s = field(climate, "u", {8U, 32U, 32U});
    forcing.v_m_s = field(climate, "v", {8U, 32U, 32U});
    forcing.ocean_east_volume_m3_s = field(climate, "ocean_east_volume", {8U, 32U, 32U});
    forcing.ocean_north_volume_m3_s = field(climate, "ocean_north_volume", {8U, 32U, 32U});
    forcing.ocean_evap_kg_m2_s = field(climate, "ocean_evap_kg_m2_s", {8U, 32U, 32U});
    auto const& rh = field(climate, "layer_vapor_RH", {8U, 2U, 32U, 32U});
    forcing.layer0_vapor_RH.resize(8U * 1024U);
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        std::copy_n(rh.begin() + static_cast<std::vector<double>::difference_type>(phase * 2U * 1024U), 1024U,
            forcing.layer0_vapor_RH.begin() + static_cast<std::vector<double>::difference_type>(phase * 1024U));
    }
    return forcing;
}

} // namespace

std::expected<V10WaterPreparation, V9WaterFailure> prepareV10Water(
    std::span<double const> canonical_l0_m, V9ClimateResult const& climate,
    V10FoundationBasis const& foundation, std::stop_token cancellation,
    std::function<void(V9WaterProgress)> progress
)
{
    if (canonical_l0_m.size() != 65'536U || std::ranges::any_of(canonical_l0_m, [](double value) {
        return !std::isfinite(value);
    }))
    {
        return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput,
            "V10 water requires finite canonical 256-square L0"});
    }
    if (cancellation.stop_requested())
    {
        return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::Cancelled, "V10 water preparation cancelled"});
    }
    try
    {
        auto forcing = makeForcing(climate);
        auto water = generateV9Water(canonical_l0_m, forcing, cancellation, std::move(progress));
        if (!water)
        {
            return std::unexpected(water.error());
        }
        auto refinement = V9WaterRefinement::create(water->equilibrium, water->routing);
        if (!refinement)
        {
            return std::unexpected(refinement.error());
        }
        V9WaterQueryModel const model{
            .base = [&foundation](double x, double y) { return sampleV10FoundationHeight(foundation, x, y); },
            .reaches = refinement->reaches(),
            .contacts = refinement->protectedContacts(),
            .progress = [&cancellation](char const*, uint64_t, uint64_t) { return !cancellation.stop_requested(); },
        };
        auto fixed_support = buildV9WaterFixedSupport(water->equilibrium, model);
        if (!fixed_support)
        {
            return std::unexpected(fixed_support.error());
        }
        auto const reaches = refinement->reaches();
        fixed_support->river_reach_order.resize(reaches.size());
        std::iota(fixed_support->river_reach_order.begin(), fixed_support->river_reach_order.end(), size_t{0U});
        std::stable_sort(fixed_support->river_reach_order.begin(), fixed_support->river_reach_order.end(),
            [&reaches](size_t a, size_t b) { return reaches[a].discharge_m3_s > reaches[b].discharge_m3_s; });
        return V10WaterPreparation{std::move(*water), std::move(*refinement), std::move(*fixed_support)};
    }
    catch (std::invalid_argument const& error)
    {
        return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput, error.what()});
    }
}

std::expected<V9WaterPointSupport, V9WaterFailure> queryV10WaterPoint(
    V10WaterPreparation const& prepared, double x_m, double y_m, double actual_bed_m
)
{
    return queryV9WaterLocalPointSupport(prepared.water.equilibrium, prepared.fixed_support,
        x_m, y_m, actual_bed_m, prepared.refinement.reaches(), prepared.fixed_support.river_reach_order);
}

std::expected<V9WaterPointSupport, V9WaterFailure> queryV10WaterPoint(
    V10WaterPreparation const& prepared, double x_m, double y_m, double actual_bed_m,
    std::span<river_field_candidate::NearestCourseGeometry const> containing_courses)
{
    // Empty reach spans select the unchanged standing-water permission path.
    auto support = queryV9WaterLocalPointSupport(prepared.water.equilibrium, prepared.fixed_support,
        x_m, y_m, actual_bed_m, {}, {});
    if (!support || support->ocean || support->lake) return support;
    auto const& reaches = prepared.refinement.reaches();
    size_t selected_index = reaches.size();
    for (auto const& course : containing_courses) {
        size_t const index = course.source_reach_index;
        if (index >= reaches.size() || !course.within_nominal_channel
            || !std::isfinite(course.course_fraction) || course.course_fraction < 0.0
            || course.course_fraction > 1.0
            || course.source_start_vertex != reaches[index].start
            || course.source_target_vertex != reaches[index].target) {
            return std::unexpected(V9WaterFailure{V9WaterFailure::Kind::InvalidInput,
                "corrected river course provenance is invalid"});
        }
        if (course.receiver_extension) continue;
        auto const& reach = reaches[index];
        // Preserve the catalog's stable discharge order at overlaps. Equal
        // discharges use source order, and the first wet course wins within a reach.
        if (selected_index != reaches.size()) {
            double const selected_discharge = reaches[selected_index].discharge_m3_s;
            if (reach.discharge_m3_s < selected_discharge
                || (reach.discharge_m3_s == selected_discharge && index >= selected_index)) continue;
        }
        double const stage = reach.water_stage0
            + course.course_fraction * (reach.water_stage1 - reach.water_stage0);
        double const depth = stage - actual_bed_m;
        float const magnitude = static_cast<float>(std::max({std::abs(stage), std::abs(actual_bed_m), 1.0}));
        double const ulp = static_cast<double>(std::nextafter(magnitude,
            std::numeric_limits<float>::infinity())) - magnitude;
        if (!(depth > std::max(1e-6, 2.0 * ulp))) continue;
        selected_index = index;
        support->river = true;
        support->river_owner_id = reach.start;
        support->water_level_m = static_cast<float>(stage);
        support->water_depth_m = static_cast<float>(depth);
    }
    return support;
}

} // namespace generation::detail
