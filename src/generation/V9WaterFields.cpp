#include "V9WaterFields.hpp"

#include <utility>

namespace generation::detail
{

std::vector<V9WaterFieldView> v9WaterNativeFields(V9WaterResult const& result)
{
    auto const& water = result.equilibrium;
    auto const& route = result.routing;
    auto const& land = result.land_inventory;
    uint64_t const n = water.resolution, phases = result.phase_count, triangles = 2 * n * n;
    std::vector<uint64_t> const grid = {n, n}, seasonal = {phases, n, n};
    std::vector<V9WaterFieldView> fields;
    fields.reserve(45);
    auto add = [&](std::string_view name, V9WaterFieldType dtype, std::vector<uint64_t> shape, auto const& data) {
        fields.push_back({name, dtype, std::move(shape), std::as_bytes(std::span(data))});
    };
    using enum V9WaterFieldType;
    add("ocean", Bool, grid, water.ocean);
    add("lake", Bool, grid, water.lake);
    add("river", Bool, grid, route.river);
    add("water_level", Float32, grid, result.water_level_m);
    add("water_depth_m", Float32, grid, result.water_depth_m);
    add("lake_id", Int32, grid, water.lake_id);
    add("downstream", Int64, grid, route.native_downstream);
    add("flow", Float32, grid, route.flow);
    add("river_discharge_m3_s", Float32, grid, route.river_discharge_m3_s);
    add("river_width_m", Float32, grid, route.river_width_m);
    add("river_depth_m", Float32, grid, route.river_depth_m);
    add("river_stage_m", Float32, grid, route.river_stage_m);
    add("river_virtual_through_lake", Bool, grid, route.river_virtual_through_lake);
    add("river_coverage_fraction", Float32, grid, route.river_coverage_fraction);
    add("salinity_g_kg", Float32, seasonal, result.salinity_g_kg);
    add("freezing_temperature_C", Float32, seasonal, result.freezing_temperature_C);
    add("temperature_C", Float32, seasonal, result.temperature_C);
    add("precipitation_mm_year", Float32, seasonal, result.precipitation_mm_year);
    add("runoff_mm_year", Float32, seasonal, result.runoff_mm_year);
    add("aridity_index", Float32, grid, land.aridity_index);
    add("biome_id", UInt8, grid, land.biome_id);
    add("land_snow_water_equivalent_m", Float32, seasonal, land.snow_water_equivalent_m);
    add("glacier_thickness_m", Float32, seasonal, land.glacier_thickness_m);
    add("sea_ice_thickness_m", Float32, seasonal, result.sea_ice_thickness_m);
    add("sea_ice_stefan_diagnostic_m", Float32, seasonal, result.sea_ice_stefan_diagnostic_m);
    add("ocean_surface_effective_temperature_C", Float32, seasonal, result.ocean_surface_effective_temperature_C);
    add("sea_ice_brine_salinity_g_kg", Float32, seasonal, result.sea_ice_brine_salinity_g_kg);
    add("equilibrium_triangle_owner", Int32, {triangles}, water.triangle_owner);
    add("equilibrium_triangle_stage_m", Float64, {triangles}, water.triangle_stage_m);
    add("equilibrium_triangle_basin_owner", Int32, {triangles}, water.triangle_basin_owner);
    add("equilibrium_hydraulic_bed_m", Float64, grid, water.hydraulic_bed_m);
    fields.push_back({"equilibrium_domain_m", Float64, {}, std::as_bytes(std::span(&water.domain_m, 1))});
    add("equilibrium_triangle_vertex_ids", Int32, {triangles, 3}, water.triangle_vertex_ids);
    add("equilibrium_lake", Bool, grid, water.lake);
    add("equilibrium_lake_id", Int32, grid, water.lake_id);
    add("equilibrium_water_level", Float32, grid, water.water_level_m);
    add("equilibrium_water_depth_m", Float32, grid, water.water_depth_m);
    add("equilibrium_vertex_receiver", Int32, grid, water.vertex_receiver);
    add("equilibrium_vertex_outlet", Int32, grid, water.vertex_outlet);
    add("equilibrium_vertex_leaf", Int32, grid, water.vertex_leaf);
    add("equilibrium_vertex_basin_owner", Int32, grid, water.vertex_basin_owner);
    add("equilibrium_triangle_runoff_source_m3_s", Float64, {triangles}, water.triangle_runoff_source_m3_s);
    add("equilibrium_triangle_source_vertex", Int32, {triangles}, water.triangle_source_vertex);
    add("v73_body_catalogue", Float64, {water.bodies.size(), 10}, result.native_body_catalogue);
    add("v73_body_receivers", Int32, {water.bodies.size(), 2}, result.native_body_receivers);
    return fields;
}

} // namespace generation::detail
