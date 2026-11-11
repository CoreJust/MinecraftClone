#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace generation::detail
{

inline constexpr size_t V10_FOUNDATION_SIDE = 256U;
inline constexpr double V10_FOUNDATION_PERIOD_M = 65'536.0;

struct V10FoundationBasis final
{
    // Row-major [y][x], origin (0,0), 256 m spacing. These are interpolation
    // bases for the continuous V10 evaluator, never a final-height atlas.
    std::vector<double> land_magnitude_m;
    std::vector<double> ocean_magnitude_m;
    std::vector<double> coast_distance_m;
    std::vector<double> regional_relief_m;
};

// Frozen V10 contract: seed 42, 256x256 finite positive raw Platec heights.
// Throws invalid_argument for another seed/size, invalid raw values or a
// source without both land and wet support. No climate or height scaling.
[[nodiscard]]
V10FoundationBasis prepareV10Foundation(uint32_t seed, std::span<float const> raw_height);

// Periodic bilinear basis samples in L,O,d,R2 order at finite absolute XY.
// This is the sampling primitive for the separately owned V10 point evaluator.
[[nodiscard]]
std::array<double, 4> sampleV10FoundationBasis(
    V10FoundationBasis const& basis, double x_m, double y_m
);

// Canonical V10 point height above sea zero, in metres (one block per metre).
// All LODs sample this same field. Uses the generated in-memory basis and the
// frozen seed-42 Perlin equations; never interpolates a final-height atlas.
[[nodiscard]]
double sampleV10FoundationHeight(V10FoundationBasis const& basis, double x_m, double y_m);

} // namespace generation::detail
