#pragma once

#include "V10Foundation.hpp"
#include "V9Climate.hpp"
#include "V9WaterModel.hpp"
#include "V9WaterQuery.hpp"

#include <generation/RiverBedChannel.hpp>

#include <expected>
#include <functional>
#include <span>
#include <stop_token>

namespace generation::detail
{

struct V10WaterPreparation final
{
    V9WaterResult water;
    // Used only for the same producer's finite reach geometry/contact metadata;
    // its terrain-changing base/apply operations are not V10 terrain authority.
    V9WaterRefinement refinement;
    V9WaterFixedSupport fixed_support;
};

// Fresh canonical V10 L0, in metres above sea zero, and its fresh 32-square
// climate. No geometry scaling, V21 cuts, file reads or fixture forcing.
[[nodiscard]]
std::expected<V10WaterPreparation, V9WaterFailure> prepareV10Water(
    std::span<double const> canonical_l0_m, V9ClimateResult const& climate,
    V10FoundationBasis const& foundation, std::stop_token cancellation = {},
    std::function<void(V9WaterProgress)> progress = {}
);

// Actual point bed clips every standing/reach occupancy test. Fixed support is
// connected permission, not a categorical nearest-cell water mask. Diagnostics
// in fixed_support retain genuine sampled lake escapes/contact mismatches.
[[nodiscard]]
std::expected<V9WaterPointSupport, V9WaterFailure> queryV10WaterPoint(
    V10WaterPreparation const& prepared, double x_m, double y_m, double actual_bed_m
);

// Canonical presentation: retain connected standing permission, then transport
// native endpoint stages along containing corrected courses. This is not S3 flow.
[[nodiscard]]
std::expected<V9WaterPointSupport, V9WaterFailure> queryV10WaterPoint(
    V10WaterPreparation const& prepared, double x_m, double y_m, double actual_bed_m,
    std::span<river_field_candidate::NearestCourseGeometry const> containing_courses
);

} // namespace generation::detail
