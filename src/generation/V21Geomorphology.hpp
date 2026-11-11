#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace generation::detail
{

struct V21PlainFeature final
{
    uint32_t candidate_rank = 0U;
    int32_t outlet_vertex = 0;
    std::vector<int32_t> path_vertices;
    std::array<double, 2U> outlet_m{}, along{}, normal{}, inspection_center_m{};
    double length_m = 0.0;
    double actual_path_length_m = 0.0;
    double baselevel_m = 0.0;
    double source_valley_grade = 0.0;
    double graded_core_1km_fraction = 0.0;
    double sediment_export_m3 = 0.0;
    std::vector<double> interior_distance_m;
};

enum class V21PlainCandidateStatus : uint8_t
{
    Selected,
    Overlap,
    NoCanonicalInterior,
    NoInspectionCore,
    DisplacementCap,
    InsufficientMaterial,
    Exhausted,
};

struct V21PlainCandidate final
{
    uint32_t rank = 0U;
    int32_t outlet_vertex = -1;
    V21PlainCandidateStatus status = V21PlainCandidateStatus::Exhausted;
};

struct V21GeomorphologyResult final
{
    std::vector<double> evolved_bed_m;
    std::vector<double> source_proxy_bed_m;
    std::vector<V21PlainFeature> plains;
    std::vector<V21PlainCandidate> plain_ledger;
    uint32_t admissible_valleys = 0U;
};

// Approved V21 canonical L0 marine shelf and JointValleyGraph equations.
// Ocean is the largest four-connected periodic negative-bed component.
[[nodiscard]]
std::vector<double> evolveV21Terrain(
    std::span<double const> bed_m,
    std::span<uint8_t const> ocean,
    std::stop_token cancellation = {}
);

// Input is the relief-transformed canonical H0 in source metres, at 256 m
// vertices. Fixed approved defaults: up to four plains from 64 candidates,
// 200 m cut and 40 m fill. No climate or occupied-water state is consumed.
[[nodiscard]]
V21GeomorphologyResult prepareV21Geomorphology(
    std::span<double const> canonical_h0_m,
    std::stop_token cancellation = {}
);

[[nodiscard]]
double v21PlainWeight(V21PlainFeature const& plain, double x_m, double y_m);

[[nodiscard]]
double v21ApplyPlainHistory(
    std::span<V21PlainFeature const> plains,
    double bed_m,
    double x_m,
    double y_m
);

// The regional operator applies this attenuation once to structural detail.
[[nodiscard]]
double v21PlainResidual(
    std::span<V21PlainFeature const> plains,
    double detail_m,
    double x_m,
    double y_m
);

// Terrain-only BoundedPeriodicBicubic: harmonic slopes, zero mixed derivative,
// and one node scale from all incident Bezier control bounds. No protected
// water-contact vertices can be inferred from the evolved bed alone.
[[nodiscard]]
double v21InterpolateEvolvedBed(
    std::span<double const> evolved_bed_m,
    double x_m,
    double y_m,
    std::span<uint8_t const> protected_vertices = {}
);

[[nodiscard]]
double v21PlainComposedBase(
    std::span<double const> evolved_bed_m,
    std::span<V21PlainFeature const> plains,
    double x_m,
    double y_m,
    std::span<uint8_t const> protected_vertices = {}
);

} // namespace generation::detail
