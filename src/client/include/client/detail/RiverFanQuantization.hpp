#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace client {

struct RiverBedMesh;
struct TerrainLodRiverGeometryAudit;

} // namespace client

namespace client::detail {

struct RiverPslgFailure;

// One internal declaration is shared by the producer and captured graph tests.
bool repairRiverFanQuantization(
    RiverBedMesh const& original,
    std::span<std::array<double, 2> const> const anchors,
    std::span<std::array<uint32_t, 2> const> const constraints,
    std::span<std::array<uint32_t, 2> const> const fan_radials,
    std::array<double, 2> const minimum,
    std::array<double, 2> const maximum,
    std::vector<std::array<double, 3>>& positions,
    std::vector<std::array<uint32_t, 3>>& triangles,
    std::vector<size_t>& sector_ids,
    std::span<std::array<int64_t, 2> const> const sector_seeds,
    std::function<bool(std::array<double, 2>, size_t, size_t)> const& equivalent_state,
    TerrainLodRiverGeometryAudit& audit,
    std::string& failure_reason,
    RiverPslgFailure* failure_capture);

} // namespace client::detail
