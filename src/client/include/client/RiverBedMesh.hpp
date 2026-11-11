#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace client {

struct RiverBedMesh final {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
};

// Coordinates are metres. The rectangle is inclusive and must have positive area.
// Edges index anchors. Exact duplicate points and edges are merged. Invalid inputs
// and non-finite heights throw std::invalid_argument; callback exceptions propagate.
// Constraint incidence uses a crop-scale floating-point roundoff tolerance.
// Every output vertex, including constraint intersections, is sampled at its original-world XY.
[[nodiscard]] RiverBedMesh buildRiverBedMesh(
    std::array<double, 2> const& minimum,
    std::array<double, 2> const& maximum,
    std::span<std::array<double, 2> const> anchors,
    std::span<std::array<uint32_t, 2> const> edges,
    std::function<double(double, double)> const& height_at);

} // namespace client
