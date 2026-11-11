#pragma once

#include "V9Terrain.hpp"

#include <cstdint>
#include <stop_token>
#include <vector>

namespace generation::detail
{

struct V21TerrainPreparation final
{
    // The selected source mapping, controls and registered structural layers.
    // final_bed_vertices_m is empty: evolution must consume canonical_h0 below.
    V9TerrainResult native;
    std::vector<double> canonical_d3_vertices_m;
    std::vector<double> lowpass_d3_vertices_m;
    std::vector<double> canonical_h0_vertices_m;
};

// Prepares the reviewed V21 native hierarchy at 256 m vertices. The caller owns
// source generation/provenance and the later relief, evolution, plains and metric
// stages. Source height units remain metres; no game-height mapping is applied.
[[nodiscard]]
V21TerrainPreparation prepareV21Terrain(
    uint64_t user_seed,
    V9PlatecSourceResult const& source,
    std::stop_token cancellation = {},
    V9TerrainProgressCallback progress = {}
);

} // namespace generation::detail
