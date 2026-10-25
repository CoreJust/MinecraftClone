#pragma once
#include "V9WaterRefinement.hpp"

#include <functional>

namespace generation::detail
{
struct V9WaterQueryModel final
{
    std::function<double(double, double)> base;
    std::span<V9RefinementReach const> reaches;
    std::span<V9ProtectedContact const> contacts;
    // Called between bounded phases; false requests cancellation.
    std::function<bool(char const*, uint64_t, uint64_t)> progress;
};
// The refinement must outlive this borrowed query view.
[[nodiscard]] V9WaterQueryModel makeV9WaterQueryModel(V9WaterRefinement const&);
struct V9WaterQueryEscape final
{
    int32_t body_id = 0;
    double stage_m = 0, canonical_spill_m = 0, sampled_escape_upper_bound_m = 0;
    std::array<double, 2> from_xy_m{}, to_xy_m{};
};
struct V9WaterQueryPermission final
{
    int32_t body_id = 0;
    double stage_m = 0;
    std::vector<uint8_t> permission;
};
struct V9WaterFixedSupport final
{
    uint32_t size = 0;
    std::vector<V9WaterQueryPermission> bodies;
    std::vector<V9WaterQueryEscape> escape_failures;
    std::vector<double> contact_actual_height_m;
    double maximum_contact_error_m = 0;
    bool contacts_pass = true;
};
struct V9WaterQueryGrid final
{
    uint32_t rows = 0, columns = 0;
    double origin_x_m = 0, origin_y_m = 0, spacing_m = 0;
    std::span<double const> bed_m;
};
struct V9WaterLocalSupport final
{
    std::vector<uint8_t> ocean, lake, river;
    std::vector<int32_t> lake_id, river_owner_id;
    std::vector<float> water_level_m, water_depth_m;
};
struct V9WaterQueryInventory final
{
    int32_t body_id = 0;
    double query_area_m2 = 0, query_volume_m3 = 0;
    bool complete_catchment_query = false;
};
struct V9WaterQueryMesh final
{
    std::vector<V9WaterSurfaceTriangle> triangles;
    std::vector<uint8_t> kind;
    std::vector<V9WaterQueryInventory> inventory;
    uint64_t river_candidate_clips = 0;
};
[[nodiscard]] std::expected<V9WaterFixedSupport, V9WaterFailure>
buildV9WaterFixedSupport(V9WaterEquilibrium const&, V9WaterQueryModel const& = {});
[[nodiscard]] std::expected<V9WaterLocalSupport, V9WaterFailure>
queryV9WaterLocalSupport(V9WaterEquilibrium const&, V9WaterFixedSupport const&, V9WaterQueryGrid const&,
                         V9WaterQueryModel const& = {});
[[nodiscard]] std::expected<V9WaterQueryMesh, V9WaterFailure>
queryV9WaterMesh(V9WaterEquilibrium const&, V9WaterFixedSupport const&, V9WaterQueryGrid const&,
                 V9WaterQueryModel const& = {}, uint64_t max_triangles = 150'000,
                 uint64_t max_output_triangles = 300'000);
} // namespace generation::detail
