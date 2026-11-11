#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace generation::detail
{

struct V9WaterRates final
{
    // Registered annual-mean rates at canonical vertices, in m/s.
    std::vector<double> evaporation_m_s;
    std::vector<double> precipitation_m_s;
    std::vector<double> runoff_m_s;
};

struct V9WaterNode final
{
    int32_t left = -1;
    int32_t right = -1;
    int32_t parent = -1;
    int32_t entry_left = -1;
    int32_t entry_right = -1;
    int32_t pit_vertex = -1;
    int32_t spill_receiver_vertex = -1;
    double merge_height_m = 0.0;
    double spill_height_m = 0.0;
};

struct V9WaterBody final
{
    int32_t id = 0;
    double stage_m = 0.0;
    double spill_m = 0.0;
    double area_m2 = 0.0;
    double volume_m3 = 0.0;
    double source_m3_s = 0.0;
    double evaporation_m3_s = 0.0;
    double precipitation_m3_s = 0.0;
    double replaced_runoff_m3_s = 0.0;
    double overflow_m3_s = 0.0;
    bool intermittent_film = false;
};

struct V9WaterTransfer final
{
    int32_t source_node = 0;
    int32_t target_node = 0;
    int32_t entry_vertex = -1;
    double rate_m3_s = 0.0;
};

struct V9WaterEquilibrium final
{
    uint32_t resolution = 0;
    double domain_m = 65'536.0;
    std::vector<double> hydraulic_bed_m;
    std::vector<uint8_t> ocean;
    std::vector<V9WaterNode> topology;
    std::vector<std::array<int32_t, 3>> triangle_vertex_ids;
    std::vector<int32_t> triangle_owner;
    std::vector<int32_t> triangle_basin_owner;
    std::vector<double> triangle_stage_m;
    std::vector<double> triangle_runoff_source_m3_s;
    std::vector<int32_t> triangle_source_vertex;
    std::vector<int32_t> vertex_receiver;
    std::vector<int32_t> vertex_leaf;
    std::vector<int32_t> vertex_outlet;
    std::vector<int32_t> vertex_basin_owner;
    std::vector<int32_t> lake_id;
    std::vector<uint8_t> lake;
    std::vector<float> water_level_m;
    std::vector<float> water_depth_m;
    std::vector<V9WaterBody> bodies;
    std::vector<V9WaterTransfer> spill_transfers;
    double runoff_vertex_mean_m3_s = 0.0;
    double runoff_source_m3_s = 0.0;
    double ocean_export_m3_s = 0.0;
    double water_residual_m3_s = 0.0;
};

struct V9WaterFailure final
{
    enum class Kind : uint8_t
    {
        InvalidInput,
        Cancelled,
        MarineFailure,
        MissingMarineOutlet,
        CyclicReceivers,
        BudgetMismatch,
        RegionalReceiverClosureRequired,
    };
    Kind kind = Kind::InvalidInput;
    std::string message;
};

// Frozen lower-star six-neighbor periodic TIN. Vertex ties use row-major index;
// triangle ownership uses its first lowest vertex. Bed is never modified.
[[nodiscard]]
std::expected<V9WaterEquilibrium, V9WaterFailure>
solveV9WaterEquilibrium(std::span<double const> bed_m, uint32_t resolution, V9WaterRates const& rates,
                        double domain_m = 65'536.0);

struct V9WaterSurfaceTriangle final
{
    std::array<std::array<double, 3>, 3> xyz_m;
    int32_t owner = 0;
    int32_t parent_triangle = 0;
};

// Returns the exact same-bed canonical lake surface clipped to immutable TIN.
// Fine/partial queries require complete catchments and receiver reconciliation.
[[nodiscard]]
std::expected<std::vector<V9WaterSurfaceTriangle>, V9WaterFailure>
queryV9LakeSurface(V9WaterEquilibrium const& water, std::span<double const> bed_m, uint32_t resolution,
                   double domain_m);

} // namespace generation::detail
