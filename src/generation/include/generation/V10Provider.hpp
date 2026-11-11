#pragma once

#include <generation/RiverBedField.hpp>
#include <generation/V9Provider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace generation
{

class V10RiverFieldError final : public std::runtime_error
{
public:
    explicit V10RiverFieldError(river_field_candidate::Failure const& failure)
        : std::runtime_error(failure.message), code(failure.code) {}

    river_field_candidate::FailureCode code;
};

struct V10NativeBedSample final
{
    std::array<double, 2U> xy_m;
    double foundation_m;
    double standing_stage_m;
    int32_t standing_owner;
};

// Every LOD samples the same absolute-coordinate bed and its actual wet footprint.
struct V10SurfaceSample final
{
    double elevation_m = 0.0;
    double water_level_m = 0.0;
    double temperature_celsius = 0.0;
    double precipitation_mm_year = 0.0;
    double snow_water_equivalent_m = 0.0;
    double glacier_thickness_m = 0.0;
    double sea_ice_thickness_m = 0.0;
    uint8_t biome_id = 0U;
    bool ocean = false;
    bool lake = false;
    bool river = false;
};

struct V10WaterGeometrySample final
{
    double elevation_m = 0.0;
    double water_level_m = 0.0;
    bool ocean = false;
    bool lake = false;
    bool river = false;
};

struct V10FineTerrainMetadata final
{
    std::string source_id;
    std::string parent_identity_sha256;
    std::string field_identity_sha256;
    std::string l2_identity_sha256;
    std::string l3_identity_sha256;
    std::string plan_sha256;
    size_t prepared_parent_queries = 0U;
    size_t mountain_site_count = 0U;
    size_t valley_network_count = 0U;
    size_t regional_site_count = 0U;
    std::string regional_draws_sha256;
    std::string regional_feature_sha256;
};

// Surface material policy only; source inventories and terrain geometry stay unchanged.
enum class V10SurfaceCover : uint8_t
{
    None,
    Snow,
    GlacierIce,
    SeaIce,
};

struct V10SurfaceQueryGrid final
{
    uint32_t rows = 0U;
    uint32_t columns = 0U;
    double origin_x_m = 0.0;
    double origin_y_m = 0.0;
    double spacing_m = 0.0;
    uint32_t lod = 0U;
    uint32_t selected_phase = 0U;
};

// Registered heights and climate/water fields are the native preparation inputs.
[[nodiscard]] std::vector<V9RegisteredFieldView> v10RegisteredFields(GlobalAtlas const& atlas);
[[nodiscard]] std::span<uint32_t const> v10CurrentPlateIds(GlobalAtlas const& atlas);
[[nodiscard]] std::span<GenerationPlateDirection const> v10PlateDirections(GlobalAtlas const& atlas);
// Preserves input coordinates and order; even an empty query validates the atlas
// and cancellation. Coordinates must be finite; standing owner zero is valid.
[[nodiscard]] std::vector<V10NativeBedSample> queryV10NativeBedSamples(
    GlobalAtlas const& atlas, std::span<std::array<double, 2U> const> xy_m,
    std::stop_token cancellation = {}
);
[[nodiscard]] std::shared_ptr<river_field_candidate::PreparedRiverField const> v10RiverBedField(
    GlobalAtlas const& atlas
);
// Corrected query bed; the native foundation remains available through the batch sampler.
[[nodiscard]] double v10L0ElevationAt(GlobalAtlas const& atlas, double world_x_m, double world_y_m);
// Evaluates the configured surface bed and exact water membership without climate sampling.
[[nodiscard]] V10WaterGeometrySample sampleV10WaterGeometry(
    GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod
);
// Evaluates bed, water occupancy and the eight-phase climate history once.
[[nodiscard]] std::array<V10SurfaceSample, 8U> sampleV10SurfacePhases(
    GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod
);
// Explicit fine queries accept LOD 2 or 3 and use the corrected river bed and
// shared water/climate path. Canonical queries select the fine field when the
// atlas enables use_v10_fine_terrain.
[[nodiscard]] V10FineTerrainMetadata v10FineTerrainMetadata(GlobalAtlas const& atlas);
[[nodiscard]] std::array<V10SurfaceSample, 8U> sampleV10FineTerrainSurfacePhases(
    GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod
);
[[nodiscard]] V10SurfaceSample sampleV10FineTerrainSurface(
    GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod,
    uint32_t selected_phase = 0U
);
// Inspector presentation policy: arithmetic mean of the eight physical phases.
// Bed, water occupancy and the all-phase biome classification remain unchanged.
[[nodiscard]] V10SurfaceSample sampleV10AnnualMeanSurface(
    GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod
);
// Surface cover is classified from the eight-phase annual mean.
[[nodiscard]] V10SurfaceCover classifyV10AnnualMeanSurfaceCover(V10SurfaceSample const& annual_mean) noexcept;
[[nodiscard]] V10SurfaceSample sampleV10Surface(
    GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod,
    uint32_t selected_phase = 0U
);
// Throws invalid_argument for malformed grids, GenerationCancelled on cancellation.
[[nodiscard]] std::vector<V10SurfaceSample> queryV10SurfaceGrid(
    GlobalAtlas const& atlas, V10SurfaceQueryGrid const& grid, std::stop_token cancellation = {}
);
[[nodiscard]] uint16_t mapV10ElevationToBlock(double source_elevation_m);

} // namespace generation
