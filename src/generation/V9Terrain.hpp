#pragma once

#include "V9PlatecSource.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace generation::detail
{

// Private source coordinates: metres on a 65,536 m periodic torus. These
// elevations are not gameplay block heights or climate-scaled elevations.
struct V9TerrainResult final
{
    std::vector<float> macro_bed_centres_m;
    std::vector<double> canonical_macro_vertices_m;
    std::vector<double> final_bed_vertices_m;
    std::vector<double> control_layers;
    std::vector<double> structural_layers;
    int32_t dendry_seed = 0;
};

enum class V9TerrainStage : uint8_t
{
    SourceMapping,
    StructuralPreparation,
    CanonicalVertices,
    ProvisionalRouting,
    TerrainEvolution,
    Complete,
};

using V9TerrainProgressCallback = std::function<void(V9TerrainStage)>;

[[nodiscard]]
V9TerrainResult prepareV9Terrain(
    uint64_t user_seed,
    V9PlatecSourceResult const& source,
    std::stop_token cancellation = {},
    V9TerrainProgressCallback progress = {}
);

// Exact pinned native hierarchy, including its bounded internal caches. This
// returns H0..H3 and residual diagnostics, before watershed/water attachment.
class V9StructuralSurface final
{
public:
    explicit V9StructuralSurface(V9TerrainResult const& terrain);
    ~V9StructuralSurface();
    V9StructuralSurface(V9StructuralSurface const&) = delete;
    V9StructuralSurface& operator=(V9StructuralSurface const&) = delete;

    [[nodiscard]]
    std::array<double, 13U> evaluate(double x_m, double y_m, uint32_t lod) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace generation::detail
