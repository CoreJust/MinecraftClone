#include <generation/V9Provider.hpp>

#include "V21CourseGeometry.hpp"
#include "V21FinePlan.hpp"
#include "V21Geomorphology.hpp"
#include "V21ProviderInternal.hpp"
#include "V21Terrain.hpp"
#include "V9Climate.hpp"
#include "V9PlatecSource.hpp"
#include "V9ProviderInternal.hpp"
#include "V9Terrain.hpp"
#include "V9TerrainMorphology.hpp"
#include "V9WaterFields.hpp"
#include "V9WaterModel.hpp"
#include "V9WaterQuery.hpp"
#include "V9WaterRefinement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace generation::detail
{

struct V9ProviderState final
{
    V9PlatecSourceResult platec;
    std::vector<generation::V9PlateDirection> public_plate_directions;
    V9TerrainResult terrain;
    V9StructuralSurface structural;
    V9ClimateResult climate;
    V9WaterResult water;
    V9WaterRefinement refinement;
    V9WaterFixedSupport fixed_water_support;

    V9ProviderState(
        V9PlatecSourceResult platec_source,
        V9TerrainResult terrain_result,
        V9ClimateResult climate_result,
        V9WaterResult water_result,
        V9WaterRefinement water_refinement
    )
        : platec{std::move(platec_source)}
        , terrain{std::move(terrain_result)}
        , structural{terrain}
        , climate{std::move(climate_result)}
        , water{std::move(water_result)}
        , refinement{std::move(water_refinement)}
    {
        public_plate_directions.reserve(platec.plate_direction_snapshot.directions.size());
        for (V9PlateDirection const& direction : platec.plate_direction_snapshot.directions)
        {
            public_plate_directions.push_back({
                .plate_id = direction.plate_id,
                .x = direction.x,
                .y = direction.y,
            });
        }
    }

    [[nodiscard]]
    double surface(double x_m, double y_m, uint32_t lod) const
    {
        auto const values = structural.evaluate(x_m, y_m, lod);
        return refinement.apply(values[0U], values[lod], x_m, y_m, lod);
    }
};

struct V21ProviderState final
{
    V9PlatecSourceResult platec;
    std::vector<generation::V9PlateDirection> public_plate_directions;
    V9TerrainResult terrain;
    std::vector<double> canonical_h0_vertices_m;
    std::vector<double> evolved_bed_vertices_m;
    std::vector<V21PlainFeature> plains;
    V9StructuralSurface structural;
    V9ClimateResult climate;
    V9WaterResult water;
    V9WaterRefinement refinement;
    V9WaterFixedSupport fixed_water_support;
    std::vector<V21CourseContact> scene_contacts;
    std::unique_ptr<V21FineProvider> fine_provider = nullptr;
    std::optional<V21SurfaceQueryFailure> fine_provider_failure;

    V21ProviderState(
        V9PlatecSourceResult platec_source,
        V9TerrainResult terrain_result,
        std::vector<double> canonical_h0,
        std::vector<double> evolved_bed,
        std::vector<V21PlainFeature> plain_features,
        V9ClimateResult climate_result,
        V9WaterResult water_result,
        V9WaterRefinement water_refinement
    )
        : platec{std::move(platec_source)}
        , terrain{std::move(terrain_result)}
        , canonical_h0_vertices_m{std::move(canonical_h0)}
        , evolved_bed_vertices_m{std::move(evolved_bed)}
        , plains{std::move(plain_features)}
        , structural{terrain}
        , climate{std::move(climate_result)}
        , water{std::move(water_result)}
        , refinement{std::move(water_refinement)}
    {
        public_plate_directions.reserve(platec.plate_direction_snapshot.directions.size());
        for (V9PlateDirection const& direction : platec.plate_direction_snapshot.directions)
        {
            public_plate_directions.push_back({
                .plate_id = direction.plate_id,
                .x = direction.x,
                .y = direction.y,
            });
        }
    }

    [[nodiscard]]
    double plainBase(double x_m, double y_m) const;

    [[nodiscard]]
    double protect(double profile_m, double x_m, double y_m) const;

    [[nodiscard]]
    double surface(double x_m, double y_m, uint32_t lod) const;
};

namespace
{

constexpr uint32_t NATIVE_RESOLUTION = 256U;
constexpr uint32_t CLIMATE_RESOLUTION = 32U;
constexpr uint32_t PHASE_COUNT = 8U;
constexpr uint32_t CLIMATE_CELL_COUNT = CLIMATE_RESOLUTION * CLIMATE_RESOLUTION;
constexpr uint32_t NATIVE_CELL_COUNT = NATIVE_RESOLUTION * NATIVE_RESOLUTION;
constexpr double WORLD_METRES = 65'536.0;

constexpr std::array<std::string_view, 95U> REGISTERED_FIELD_NAMES{
    "absorbed_sw_W_m2", "aridity_index", "atmosphere_capacity", "base_u", "bed_m",
    "biome_id", "bucket_capacity_kg_m2", "bucket_kg_m2", "carrier_flux_x",
    "cloud_evap_kg_m2_s", "cloud_transport_tendency_kg_m2_s", "cloud_water_kg_m2",
    "column_vapor_RH", "combined_energy_residual", "condensation_kg_m2_s", "downstream",
    "dry_heat_export_W_m2", "elevation", "flow", "freezing_temperature_C",
    "glacier_thickness_m", "ground_m", "lake", "lake_id", "land_evap_kg_m2_s",
    "land_snow_potential", "land_snow_water_equivalent_m", "layer_cloud_kg_m2",
    "layer_mass", "layer_rain_kg_m2_s", "layer_saturation_kg_m2", "layer_vapor_RH",
    "layer_vapor_kg_m2", "layer_water_kg_m2", "local_effective_C", "ocean",
    "ocean_east_volume", "ocean_evap_kg_m2_s", "ocean_freezing_potential",
    "ocean_heat_export_W_m2", "ocean_heat_residual", "ocean_north_volume",
    "ocean_surface_effective_temperature_C", "ocean_u", "ocean_v",
    "omitted_latent_heating_W_m2", "outgoing_longwave_W_m2",
    "overturning_lower_u_m_s", "overturning_lower_v_m_s",
    "overturning_lower_x_mass_flux", "phase_cloud_begin_kg_m2", "phase_cloud_end_kg_m2",
    "phase_soil_begin_kg_m2", "phase_soil_end_kg_m2", "phase_vapor_begin_kg_m2",
    "phase_vapor_end_kg_m2", "planetary_albedo", "precipitation_mm_year",
    "pressure_response", "qg_mean_u_m_s", "qg_mean_v_m_s", "rain_kg_m2_s",
    "reference_ps", "river", "river_coverage_fraction", "river_depth_m",
    "river_discharge_m3_s", "river_stage_m", "river_virtual_through_lake",
    "river_width_m", "runoff_kg_m2_s", "runoff_mm_year", "salinity_g_kg",
    "sea_ice_brine_salinity_g_kg", "sea_ice_stefan_diagnostic_m", "sea_ice_thickness_m",
    "surface_capacity", "temperature_C", "terrain_u", "terrain_v", "thermal_forcing",
    "thermal_residual", "thermal_storage_W_m2", "theta_C", "total_capacity", "u", "v",
    "vapor_transport_tendency_kg_m2_s", "water_depth_m", "water_level",
    "weather_base_u_m_s", "weather_base_v_m_s", "weather_u_anomaly_m_s",
    "weather_v_anomaly_m_s", "wet_fraction",
};

void checkCancelled(std::stop_token const& cancellation)
{
    if (cancellation.stop_requested())
    {
        throw GenerationCancelled{};
    }
}

[[nodiscard]]
double periodicCubic(std::span<double const> values, double world_x_m, double world_y_m)
{
    if (values.size() != NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"V21 periodic cubic input must be a 256-square vertex field"};
    }
    auto wrap = [](double coordinate) {
        double result = std::fmod(coordinate, WORLD_METRES);
        return result < 0.0 ? result + WORLD_METRES : result;
    };
    double const px = wrap(world_x_m) / 256.0;
    double const py = wrap(world_y_m) / 256.0;
    int32_t const ix = static_cast<int32_t>(std::floor(px));
    int32_t const iy = static_cast<int32_t>(std::floor(py));
    double const tx = px - ix;
    double const ty = py - iy;
    auto weights = [](double const t) {
        double const t2 = t * t;
        double const t3 = t2 * t;
        return std::array<double, 4U>{
            -.5 * t + t2 - .5 * t3,
            1.0 - 2.5 * t2 + 1.5 * t3,
            .5 * t + 2.0 * t2 - 1.5 * t3,
            -.5 * t2 + .5 * t3,
        };
    };
    auto const wx = weights(tx);
    auto const wy = weights(ty);
    double result = 0.0;
    for (int32_t j = 0; j < 4; ++j)
    {
        double row = 0.0;
        for (int32_t i = 0; i < 4; ++i)
        {
            uint32_t const x = static_cast<uint32_t>((ix + i - 1 + NATIVE_RESOLUTION) % NATIVE_RESOLUTION);
            uint32_t const y = static_cast<uint32_t>((iy + j - 1 + NATIVE_RESOLUTION) % NATIVE_RESOLUTION);
            row += wx[i] * values[static_cast<size_t>(y) * NATIVE_RESOLUTION + x];
        }
        result += wy[j] * row;
    }
    return result;
}

void reportProgress(
    ProgressCallback const& progress,
    GenerationStage stage,
    uint32_t completed,
    uint32_t total,
    std::shared_ptr<GenerationSnapshot const> snapshot = {}
)
{
    if (progress)
    {
        progress(GenerationProgress{
            .stage = stage,
            .completed = completed,
            .total = total,
            .snapshot = std::move(snapshot),
        });
    }
}

[[nodiscard]]
V9GenerationSnapshotFields makeV9SnapshotFields(
    V9PlatecSourceResult const& platec,
    std::span<double const> elevation_m
)
{
    if (elevation_m.size() != NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"v9 progress snapshot elevation does not match the native source grid"};
    }
    V9GenerationSnapshotFields fields;
    fields.source_elevation_m.reserve(NATIVE_CELL_COUNT);
    for (double const value : elevation_m)
    {
        fields.source_elevation_m.push_back(static_cast<float>(value));
    }
    fields.plate_ids = platec.current_plate_id;
    fields.plate_directions.reserve(platec.plate_direction_snapshot.directions.size());
    for (V9PlateDirection const& direction : platec.plate_direction_snapshot.directions)
    {
        fields.plate_directions.push_back({
            .plate_id = direction.plate_id,
            .x = direction.x,
            .y = direction.y,
        });
    }
    fields.plate_directions_observed_after_native_step =
        platec.plate_direction_snapshot.observed_after_native_step;
    return fields;
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV9PlatecSnapshot(V9PlatecSourceResult const& platec)
{
    if (platec.height_raw.size() != NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"v9 Platec preview does not match the native source grid"};
    }
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = GenerationStage::V9Platec;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v9.emplace();
    snapshot->v9->source_elevation_m.reserve(NATIVE_CELL_COUNT);
    for (float const raw_height : platec.height_raw)
    {
        snapshot->v9->source_elevation_m.push_back(v9PlatecHeightToSourceMetres(raw_height));
    }
    snapshot->v9->plate_ids = platec.current_plate_id;
    snapshot->v9->plate_directions.reserve(platec.plate_direction_snapshot.directions.size());
    for (V9PlateDirection const& direction : platec.plate_direction_snapshot.directions)
    {
        snapshot->v9->plate_directions.push_back({
            .plate_id = direction.plate_id,
            .x = direction.x,
            .y = direction.y,
        });
    }
    snapshot->v9->plate_directions_observed_after_native_step =
        platec.plate_direction_snapshot.observed_after_native_step;
    return snapshot;
}

[[nodiscard]]
std::vector<float> meanClimateGrid(V9ClimateField const& field)
{
    if (field.shape != std::vector<uint32_t>{PHASE_COUNT, CLIMATE_RESOLUTION, CLIMATE_RESOLUTION}
        || field.values.size() != static_cast<size_t>(PHASE_COUNT) * CLIMATE_CELL_COUNT)
    {
        throw std::logic_error{"v9 climate progress field has an unexpected source shape"};
    }
    std::vector<float> coarse_mean(CLIMATE_CELL_COUNT);
    for (uint32_t index = 0U; index < CLIMATE_CELL_COUNT; ++index)
    {
        double mean = 0.0;
        for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
        {
            mean += field.values[static_cast<size_t>(phase) * CLIMATE_CELL_COUNT + index]
                / static_cast<double>(PHASE_COUNT);
        }
        coarse_mean[index] = static_cast<float>(mean);
    }
    std::vector<float> native_mean(NATIVE_CELL_COUNT);
    for (uint32_t y = 0U; y < NATIVE_RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < NATIVE_RESOLUTION; ++x)
        {
            native_mean[static_cast<size_t>(y) * NATIVE_RESOLUTION + x] =
                coarse_mean[static_cast<size_t>(y / 8U) * CLIMATE_RESOLUTION + x / 8U];
        }
    }
    return native_mean;
}

[[nodiscard]]
std::vector<float> expandPhaseClimateGrid(V9ClimateField const& field)
{
    if (field.shape != std::vector<uint32_t>{PHASE_COUNT, CLIMATE_RESOLUTION, CLIMATE_RESOLUTION}
        || field.values.size() != static_cast<size_t>(PHASE_COUNT) * CLIMATE_CELL_COUNT)
    {
        throw std::logic_error{"V21 phase climate snapshot field has an unexpected source shape"};
    }
    std::vector<float> native_phase(PHASE_COUNT * NATIVE_CELL_COUNT);
    for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
    {
        size_t const phase_offset = static_cast<size_t>(phase) * CLIMATE_CELL_COUNT;
        size_t const native_phase_offset = static_cast<size_t>(phase) * NATIVE_CELL_COUNT;
        for (uint32_t y = 0U; y < NATIVE_RESOLUTION; ++y)
        {
            for (uint32_t x = 0U; x < NATIVE_RESOLUTION; ++x)
            {
                size_t const native_index = static_cast<size_t>(y) * NATIVE_RESOLUTION + x;
                size_t const climate_index = static_cast<size_t>(y / 8U) * CLIMATE_RESOLUTION + x / 8U;
                native_phase[native_phase_offset + native_index] =
                    static_cast<float>(field.values[phase_offset + climate_index]);
            }
        }
    }
    return native_phase;
}

[[nodiscard]]
std::vector<float> meanNativePhaseField(std::span<float const> values)
{
    if (values.size() != static_cast<size_t>(PHASE_COUNT) * NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"V21 native phase climate field does not match the eight-phase source grid"};
    }
    std::vector<float> mean(NATIVE_CELL_COUNT);
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        double sum = 0.0;
        for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
        {
            sum += static_cast<double>(values[static_cast<size_t>(phase) * NATIVE_CELL_COUNT + index]);
        }
        mean[index] = static_cast<float>(sum / static_cast<double>(PHASE_COUNT));
    }
    return mean;
}

[[nodiscard]]
V21GenerationSnapshotFields makeV21BaseSnapshotFields(
    V9PlatecSourceResult const& platec,
    std::span<double const> elevation_m
)
{
    if (elevation_m.size() != NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"V21 progress snapshot elevation does not match the native source grid"};
    }
    V21GenerationSnapshotFields fields;
    fields.source_elevation_m.reserve(NATIVE_CELL_COUNT);
    for (double const value : elevation_m)
    {
        fields.source_elevation_m.push_back(static_cast<float>(value));
    }
    fields.plate_ids = platec.current_plate_id;
    fields.plate_directions.reserve(platec.plate_direction_snapshot.directions.size());
    for (V9PlateDirection const& direction : platec.plate_direction_snapshot.directions)
    {
        fields.plate_directions.push_back({
            .plate_id = direction.plate_id,
            .x = direction.x,
            .y = direction.y,
        });
    }
    fields.plate_directions_observed_after_native_step =
        platec.plate_direction_snapshot.observed_after_native_step;
    return fields;
}

void addV21ClimateSnapshotFields(V21GenerationSnapshotFields& fields, V9ClimateResult const& climate)
{
    auto const temperature = climate.fields.find("theta_C");
    auto const wind_u = climate.fields.find("u");
    auto const wind_v = climate.fields.find("v");
    if (temperature == climate.fields.end() || wind_u == climate.fields.end() || wind_v == climate.fields.end())
    {
        throw std::logic_error{"V21 climate progress snapshot is missing temperature or wind fields"};
    }
    fields.temperature_celsius = meanClimateGrid(temperature->second);
    fields.wind_u = meanClimateGrid(wind_u->second);
    fields.wind_v = meanClimateGrid(wind_v->second);
    fields.phase_temperature_celsius = expandPhaseClimateGrid(temperature->second);

    auto const precipitation = climate.fields.find("precipitation_mm_year");
    if (precipitation != climate.fields.end())
    {
        fields.precipitation_mm_year = meanClimateGrid(precipitation->second);
        fields.phase_precipitation_mm_year = expandPhaseClimateGrid(precipitation->second);
    }
    auto const current_u = climate.fields.find("ocean_u");
    auto const current_v = climate.fields.find("ocean_v");
    if ((current_u == climate.fields.end()) != (current_v == climate.fields.end()))
    {
        throw std::logic_error{"V21 climate progress snapshot has only one ocean-current component"};
    }
    if (current_u != climate.fields.end())
    {
        fields.current_u = meanClimateGrid(current_u->second);
        fields.current_v = meanClimateGrid(current_v->second);
    }
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV21PlatecSnapshot(V9PlatecSourceResult const& platec)
{
    if (platec.height_raw.size() != NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"V21 Platec preview does not match the native source grid"};
    }
    std::vector<double> source_elevation_m;
    source_elevation_m.reserve(NATIVE_CELL_COUNT);
    for (float const raw_height : platec.height_raw)
    {
        source_elevation_m.push_back(v9PlatecHeightToSourceMetres(raw_height));
    }
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = GenerationStage::V21Platec;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v21 = makeV21BaseSnapshotFields(platec, source_elevation_m);
    return snapshot;
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV21TerrainSnapshot(
    V9PlatecSourceResult const& platec,
    std::span<double const> source_elevation_m
)
{
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = GenerationStage::V21Terrain;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v21 = makeV21BaseSnapshotFields(platec, source_elevation_m);
    return snapshot;
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV21ClimateSnapshot(
    V9PlatecSourceResult const& platec,
    std::span<double const> source_elevation_m,
    V9ClimateResult const& climate
)
{
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = GenerationStage::V21Climate;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v21 = makeV21BaseSnapshotFields(platec, source_elevation_m);
    addV21ClimateSnapshotFields(*snapshot->v21, climate);
    return snapshot;
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV21WaterSnapshot(
    V9PlatecSourceResult const& platec,
    std::span<double const> source_elevation_m,
    V9WaterResult const& water,
    V9ClimateResult const& climate
)
{
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = GenerationStage::V21Water;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v21 = makeV21BaseSnapshotFields(platec, source_elevation_m);
    addV21ClimateSnapshotFields(*snapshot->v21, climate);
    V21GenerationSnapshotFields& fields = *snapshot->v21;
    fields.temperature_celsius = meanNativePhaseField(water.temperature_C);
    fields.precipitation_mm_year = meanNativePhaseField(water.precipitation_mm_year);
    fields.water_level_blocks = water.water_level_m;
    fields.ocean_mask = water.equilibrium.ocean;
    fields.lake_mask = water.equilibrium.lake;
    fields.river_mask = water.routing.river;
    fields.biome_id = water.land_inventory.biome_id;
    fields.phase_temperature_celsius = water.temperature_C;
    fields.phase_precipitation_mm_year = water.precipitation_mm_year;
    fields.land_snow_water_equivalent_m = water.land_inventory.snow_water_equivalent_m;
    fields.glacier_thickness_m = water.land_inventory.glacier_thickness_m;
    fields.sea_ice_thickness_m = water.sea_ice_thickness_m;
    fields.ice_surface_kind.resize(NATIVE_CELL_COUNT,
        static_cast<uint8_t>(V21GenerationIceSurfaceKind::None));
    constexpr float DISPLAY_ICE_CUTOFF_M = 0.1F;
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        size_t const phase_zero = index;
        if (fields.ocean_mask[index] != 0U)
        {
            if (fields.sea_ice_thickness_m[phase_zero] >= DISPLAY_ICE_CUTOFF_M)
            {
                fields.ice_surface_kind[index] = static_cast<uint8_t>(V21GenerationIceSurfaceKind::SeaIce);
            }
        }
        else if (fields.land_snow_water_equivalent_m[phase_zero] >= DISPLAY_ICE_CUTOFF_M)
        {
            fields.ice_surface_kind[index] = static_cast<uint8_t>(V21GenerationIceSurfaceKind::Snow);
        }
        else if (fields.glacier_thickness_m[phase_zero] >= DISPLAY_ICE_CUTOFF_M)
        {
            fields.ice_surface_kind[index] = static_cast<uint8_t>(V21GenerationIceSurfaceKind::GlacierIce);
        }
    }
    return snapshot;
}

void addV9SnapshotWeather(V9GenerationSnapshotFields& fields, V9ClimateResult const& climate)
{
    fields.wind_u = meanClimateGrid(climate.fields.at("u"));
    fields.wind_v = meanClimateGrid(climate.fields.at("v"));
    fields.current_u = meanClimateGrid(climate.fields.at("ocean_u"));
    fields.current_v = meanClimateGrid(climate.fields.at("ocean_v"));
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV9ClimateSnapshot(
    V9PlatecSourceResult const& platec,
    V9TerrainResult const& terrain,
    V9ClimateResult const& climate
)
{
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = GenerationStage::V9Climate;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v9 = makeV9SnapshotFields(platec, terrain.final_bed_vertices_m);
    auto const temperature = climate.fields.find("theta_C");
    if (temperature == climate.fields.end())
    {
        throw std::logic_error{"v9 climate progress snapshot has no source theta_C field"};
    }
    snapshot->v9->temperature_celsius = meanClimateGrid(temperature->second);
    addV9SnapshotWeather(*snapshot->v9, climate);
    return snapshot;
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV9WaterSnapshot(
    V9PlatecSourceResult const& platec,
    V9TerrainResult const& terrain,
    V9WaterResult const& water,
    V9ClimateResult const& climate,
    GenerationStage const stage
)
{
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = stage;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v9 = makeV9SnapshotFields(platec, terrain.final_bed_vertices_m);
    addV9SnapshotWeather(*snapshot->v9, climate);
    snapshot->v9->temperature_celsius.resize(NATIVE_CELL_COUNT);
    snapshot->v9->precipitation_mm_year.resize(NATIVE_CELL_COUNT);
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        double temperature = 0.0;
        double precipitation = 0.0;
        for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
        {
            size_t const source_index = static_cast<size_t>(phase) * NATIVE_CELL_COUNT + index;
            temperature += static_cast<double>(water.temperature_C[source_index]) / PHASE_COUNT;
            precipitation += static_cast<double>(water.precipitation_mm_year[source_index]) / PHASE_COUNT;
        }
        snapshot->v9->temperature_celsius[index] = static_cast<float>(temperature);
        snapshot->v9->precipitation_mm_year[index] = static_cast<float>(precipitation);
    }
    snapshot->v9->water_level_m = water.water_level_m;
    snapshot->v9->ocean_mask = water.equilibrium.ocean;
    snapshot->v9->lake_mask = water.equilibrium.lake;
    snapshot->v9->river_mask = water.routing.river;
    snapshot->v9->ice_surface_kind.resize(NATIVE_CELL_COUNT,
        static_cast<uint8_t>(V9GenerationIceSurfaceKind::None));
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        bool has_land_snow = false;
        bool has_water_ice = false;
        for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
        {
            size_t const source_index = static_cast<size_t>(phase) * NATIVE_CELL_COUNT + index;
            has_land_snow = has_land_snow
                || water.land_inventory.snow_water_equivalent_m[source_index] > 0.0F
                || water.land_inventory.glacier_thickness_m[source_index] > 0.0F;
            has_water_ice = has_water_ice || water.sea_ice_thickness_m[source_index] > 0.0F;
        }
        if (has_water_ice)
        {
            snapshot->v9->ice_surface_kind[index] =
                static_cast<uint8_t>(V9GenerationIceSurfaceKind::WaterIce);
        }
        else if (has_land_snow)
        {
            snapshot->v9->ice_surface_kind[index] =
                static_cast<uint8_t>(V9GenerationIceSurfaceKind::LandSnow);
        }
    }
    return snapshot;
}

[[nodiscard]]
V9ClimateField const& climateField(V9ClimateResult const& climate, std::string_view name)
{
    auto const found = climate.fields.find(std::string{name});
    if (found == climate.fields.end())
    {
        throw std::runtime_error{"v9 climate output is missing required source field " + std::string{name}};
    }
    return found->second;
}

[[nodiscard]]
V9WaterForcing makeWaterForcing(V9ClimateResult const& climate)
{
    V9WaterForcing forcing;
    forcing.resolution = CLIMATE_RESOLUTION;
    forcing.phase_count = PHASE_COUNT;
    forcing.horizontal_metric_scale = 250.0;
    forcing.ground_m = climateField(climate, "ground_m").values;
    forcing.reference_ps = climateField(climate, "reference_ps").values;
    forcing.wet_fraction = climateField(climate, "wet_fraction").values;
    forcing.local_effective_C = climateField(climate, "local_effective_C").values;
    forcing.theta_C = climateField(climate, "theta_C").values;
    forcing.rain_kg_m2_s = climateField(climate, "rain_kg_m2_s").values;
    forcing.runoff_kg_m2_s = climateField(climate, "runoff_kg_m2_s").values;
    forcing.u_m_s = climateField(climate, "u").values;
    forcing.v_m_s = climateField(climate, "v").values;
    forcing.ocean_east_volume_m3_s = climateField(climate, "ocean_east_volume").values;
    forcing.ocean_north_volume_m3_s = climateField(climate, "ocean_north_volume").values;
    forcing.ocean_evap_kg_m2_s = climateField(climate, "ocean_evap_kg_m2_s").values;

    V9ClimateField const& layer_vapor_rh = climateField(climate, "layer_vapor_RH");
    size_t const layer_stride = static_cast<size_t>(CLIMATE_CELL_COUNT);
    forcing.layer0_vapor_RH.resize(static_cast<size_t>(PHASE_COUNT) * layer_stride);
    for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
    {
        size_t const source_offset = static_cast<size_t>(phase) * 2U * layer_stride;
        size_t const target_offset = static_cast<size_t>(phase) * layer_stride;
        std::span<double const> const source_values{layer_vapor_rh.values};
        std::span<double> const target_values{forcing.layer0_vapor_RH};
        std::copy_n(
            source_values.subspan(source_offset, layer_stride).begin(),
            layer_stride,
            target_values.subspan(target_offset, layer_stride).begin()
        );
    }
    return forcing;
}

[[nodiscard]]
uint32_t climateProgressPosition(V9ClimateProgress const progress)
{
    constexpr uint32_t THERMAL_WORK = 1U;
    constexpr uint32_t WEATHER_WORK = 180U;
    constexpr uint32_t MOISTURE_WORK = 1'080U;
    switch (progress.stage)
    {
    case V9ClimateProgress::Stage::Thermal:
        return std::min(progress.completed, THERMAL_WORK);
    case V9ClimateProgress::Stage::Weather:
        return THERMAL_WORK + std::min(progress.completed, WEATHER_WORK);
    case V9ClimateProgress::Stage::Moisture:
        return THERMAL_WORK + WEATHER_WORK + std::min(progress.completed, MOISTURE_WORK);
    case V9ClimateProgress::Stage::Complete:
        return THERMAL_WORK + WEATHER_WORK + MOISTURE_WORK;
    }
    return 0U;
}

[[nodiscard]]
V9FieldRegistration registrationForShape(std::span<uint32_t const> shape)
{
    if ((shape.size() == 2U && shape[0] == NATIVE_RESOLUTION && shape[1] == NATIVE_RESOLUTION)
        || (shape.size() == 3U && shape[1] == NATIVE_RESOLUTION && shape[2] == NATIVE_RESOLUTION))
    {
        return V9FieldRegistration::PeriodicVertex;
    }
    if ((shape.size() == 2U && shape[0] == CLIMATE_RESOLUTION && shape[1] == CLIMATE_RESOLUTION)
        || (shape.size() == 3U && shape[1] == CLIMATE_RESOLUTION && shape[2] == CLIMATE_RESOLUTION))
    {
        return V9FieldRegistration::PeriodicCellCentre;
    }
    return V9FieldRegistration::Native;
}

[[nodiscard]]
V9FieldType convertFieldType(V9WaterFieldType type)
{
    switch (type)
    {
    case V9WaterFieldType::Bool:
        return V9FieldType::Bool;
    case V9WaterFieldType::UInt8:
        return V9FieldType::UInt8;
    case V9WaterFieldType::Int32:
        return V9FieldType::Int32;
    case V9WaterFieldType::Int64:
        return V9FieldType::Int64;
    case V9WaterFieldType::Float32:
        return V9FieldType::Float32;
    case V9WaterFieldType::Float64:
        return V9FieldType::Float64;
    }
    throw std::logic_error{"unsupported v9 water field dtype"};
}

template <typename T>
[[nodiscard]]
std::span<T const> typedWaterValues(V9WaterFieldView const& field)
{
    if (field.bytes.size() % sizeof(T) != 0U)
    {
        throw std::logic_error{"v9 water field byte size does not match its declared dtype"};
    }
    return {
        reinterpret_cast<T const*>(field.bytes.data()),
        field.bytes.size() / sizeof(T),
    };
}

[[nodiscard]]
V9FieldValues waterValues(V9WaterFieldView const& field)
{
    switch (field.dtype)
    {
    case V9WaterFieldType::Bool:
    case V9WaterFieldType::UInt8:
        return typedWaterValues<uint8_t>(field);
    case V9WaterFieldType::Int32:
        return typedWaterValues<int32_t>(field);
    case V9WaterFieldType::Int64:
        return typedWaterValues<int64_t>(field);
    case V9WaterFieldType::Float32:
        return typedWaterValues<float>(field);
    case V9WaterFieldType::Float64:
        return typedWaterValues<double>(field);
    }
    throw std::logic_error{"unsupported v9 water field dtype"};
}

template <typename ProviderState>
[[nodiscard]]
std::vector<V9RegisteredFieldView> makeRegisteredFields(ProviderState const& state)
{
    std::vector<V9WaterFieldView> water_fields = v9WaterNativeFields(state.water);
    std::vector<V9RegisteredFieldView> views;
    views.reserve(REGISTERED_FIELD_NAMES.size());
    for (std::string_view const name : REGISTERED_FIELD_NAMES)
    {
        if (name == "elevation")
        {
            views.push_back({
                .name = name,
                .type = V9FieldType::Float64,
                .registration = V9FieldRegistration::PeriodicVertex,
                .shape = {NATIVE_RESOLUTION, NATIVE_RESOLUTION},
                .values = std::span<double const>{state.terrain.final_bed_vertices_m},
            });
            continue;
        }

        auto const climate = state.climate.fields.find(std::string{name});
        if (climate != state.climate.fields.end())
        {
            auto const& field = climate->second;
            views.push_back({
                .name = climate->first,
                .type = V9FieldType::Float64,
                .registration = registrationForShape(field.shape),
                .shape = field.shape,
                .values = std::span<double const>{field.values},
            });
            continue;
        }

        auto const water = std::find_if(water_fields.begin(), water_fields.end(), [name](V9WaterFieldView const& field) {
            return field.name == name;
        });
        if (water == water_fields.end())
        {
            throw std::runtime_error{"native production field registry is missing source field " + std::string{name}};
        }
        std::vector<uint32_t> shape;
        shape.reserve(water->shape.size());
        for (uint64_t const extent : water->shape)
        {
            if (extent > std::numeric_limits<uint32_t>::max())
            {
                throw std::logic_error{"v9 registered field extent exceeds uint32"};
            }
            shape.push_back(static_cast<uint32_t>(extent));
        }
        views.push_back({
            .name = water->name,
            .type = convertFieldType(water->dtype),
            .registration = registrationForShape(shape),
            .shape = std::move(shape),
            .values = waterValues(*water),
        });
    }
    if (views.size() != REGISTERED_FIELD_NAMES.size())
    {
        throw std::logic_error{"native production registry count differs from the frozen 95-field contract"};
    }
    return views;
}

[[nodiscard]]
std::shared_ptr<GlobalAtlas const> makeV21Atlas(
    GenerationConfiguration const& configuration,
    std::shared_ptr<V21ProviderState const> provider
)
{
    GlobalAtlas atlas;
    atlas.configuration = configuration;
    atlas.v21_provider = std::move(provider);
    atlas.block_sea_level = V21_BLOCK_SEA_LEVEL;
    atlas.max_height = V21_MAX_HEIGHT;
    return std::make_shared<GlobalAtlas const>(std::move(atlas));
}

void verifyV21MappedDomain(GlobalAtlas const& atlas, std::stop_token const& cancellation)
{
    auto const& provider = *atlas.v21_provider;
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        if ((index & 255U) == 0U)
        {
            checkCancelled(cancellation);
        }
        static_cast<void>(mapV21ElevationToBlock(provider.terrain.final_bed_vertices_m[index]));
        static_cast<void>(mapV21WaterLevelToBlock(provider.water.water_level_m[index] / 0.34));
        if (provider.water.routing.river[index] != 0U)
        {
            static_cast<void>(mapV21WaterLevelToBlock(provider.water.routing.river_stage_m[index] / 0.34));
        }
    }
    for (V9WaterBody const& body : provider.water.equilibrium.bodies)
    {
        checkCancelled(cancellation);
        static_cast<void>(mapV21WaterLevelToBlock(body.stage_m / 0.34));
        static_cast<void>(mapV21WaterLevelToBlock(body.spill_m / 0.34));
    }
    constexpr std::array<std::array<uint32_t, 2U>, 4U> QUERY_POINTS{{
        {0U, 0U}, {NATIVE_RESOLUTION / 4U, NATIVE_RESOLUTION / 4U},
        {NATIVE_RESOLUTION / 2U, NATIVE_RESOLUTION / 2U},
        {3U * NATIVE_RESOLUTION / 4U, 3U * NATIVE_RESOLUTION / 4U},
    }};
    for (auto const& point : QUERY_POINTS)
    {
        checkCancelled(cancellation);
        uint32_t const x = point[0U] * 256U;
        uint32_t const y = point[1U] * 256U;
        for (uint32_t lod = 0U; lod < 4U; ++lod)
        {
            V9SurfaceSample const sample = sampleV21Surface(atlas, x, y, lod);
            static_cast<void>(mapV21ElevationToBlock(sample.elevation_m));
            static_cast<void>(mapV21WaterLevelToBlock(sample.water_level_m));
        }
    }
}

[[nodiscard]]
std::shared_ptr<GlobalAtlas const> makeAtlas(
    GenerationConfiguration const& configuration,
    std::shared_ptr<V9ProviderState const> provider
)
{
    GlobalAtlas atlas;
    atlas.configuration = configuration;
    atlas.v9_provider = std::move(provider);
    atlas.block_sea_level = V9_BLOCK_SEA_LEVEL;
    atlas.max_height = V9_MAX_HEIGHT;
    return std::make_shared<GlobalAtlas const>(std::move(atlas));
}

void verifyMappedDomain(GlobalAtlas const& atlas, std::stop_token const& cancellation)
{
    auto const ids = v9CurrentPlateIds(atlas);
    if (ids.size() != NATIVE_CELL_COUNT)
    {
        throw std::logic_error{"v9 provider lost the registered uint32 Platec ownership map"};
    }

    auto const& provider = *atlas.v9_provider;
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        if ((index & 255U) == 0U)
        {
            checkCancelled(cancellation);
        }
        static_cast<void>(mapV9ElevationToBlock(provider.terrain.final_bed_vertices_m[index]));
        static_cast<void>(mapV9WaterLevelToBlock(provider.water.water_level_m[index]));
        if (provider.water.routing.river[index] != 0U)
        {
            static_cast<void>(mapV9WaterLevelToBlock(provider.water.routing.river_stage_m[index]));
        }
    }
    for (V9WaterBody const& body : provider.water.equilibrium.bodies)
    {
        checkCancelled(cancellation);
        static_cast<void>(mapV9WaterLevelToBlock(body.stage_m));
        static_cast<void>(mapV9WaterLevelToBlock(body.spill_m));
    }

    constexpr std::array<std::array<uint32_t, 2U>, 4U> QUERY_POINTS{{
        {0U, 0U}, {NATIVE_RESOLUTION / 4U, NATIVE_RESOLUTION / 4U},
        {NATIVE_RESOLUTION / 2U, NATIVE_RESOLUTION / 2U},
        {3U * NATIVE_RESOLUTION / 4U, 3U * NATIVE_RESOLUTION / 4U},
    }};
    for (auto const& point : QUERY_POINTS)
    {
        checkCancelled(cancellation);
        uint32_t const x = point[0U] * 256U;
        uint32_t const y = point[1U] * 256U;
        for (uint32_t lod = 0U; lod < 4U; ++lod)
        {
            V9SurfaceSample const sample = sampleV9Surface(atlas, x, y, lod);
            static_cast<void>(mapV9ElevationToBlock(sample.elevation_m));
            static_cast<void>(mapV9WaterLevelToBlock(sample.water_level_m));
        }
    }
}

} // namespace

double V21ProviderState::plainBase(double x_m, double y_m) const
{
    return v21PlainComposedBase(
        evolved_bed_vertices_m, plains, x_m, y_m, refinement.v21InterpolationConstraints()
    );
}

double V21ProviderState::protect(double profile_m, double x_m, double y_m) const
{
    double contact_weight = 1.0;
    auto periodic_delta = [](double coordinate) {
        double wrapped = std::fmod(coordinate + WORLD_METRES / 2.0, WORLD_METRES);
        if (wrapped < 0.0)
        {
            wrapped += WORLD_METRES;
        }
        return wrapped - WORLD_METRES / 2.0;
    };
    for (V21CourseContact const& contact : scene_contacts)
    {
        double const dx = periodic_delta(x_m - contact.xy_m[0U]);
        double const dy = periodic_delta(y_m - contact.xy_m[1U]);
        double const u = std::clamp(std::hypot(dx, dy) / contact.radius_m, 0.0, 1.0);
        double const remaining = 1.0 - u * u;
        contact_weight *= 1.0 - remaining * remaining * remaining;
    }
    double const base = plainBase(x_m, y_m);
    return base + (profile_m - base) * contact_weight;
}

double V21ProviderState::surface(double x_m, double y_m, uint32_t lod) const
{
    auto const native_levels = structural.evaluate(x_m, y_m, lod);
    double const plain_composed_base = plainBase(x_m, y_m);
    double const transformed_h0 = periodicCubic(canonical_h0_vertices_m, x_m, y_m);
    double const source_detail = native_levels[lod] - transformed_h0;
    double const detail = v21PlainResidual(plains, source_detail, x_m, y_m);
    return protect(plain_composed_base + detail, x_m, y_m);
}

[[nodiscard]]
std::expected<std::unique_ptr<V21FineProvider>, V21SurfaceQueryFailure> makeV21FineProvider(
    std::shared_ptr<V21ProviderState> const& provider,
    std::stop_token cancellation
)
{
    std::weak_ptr<V21ProviderState> const weak_provider = provider;
    auto const standing = [weak_provider](std::span<V21FinePoint const> points) {
        auto locked = weak_provider.lock();
        if (!locked)
        {
            throw std::runtime_error{"V21 provider expired during regional geometry setup"};
        }
        std::vector<V21FineStanding> result;
        result.reserve(points.size());
        for (V21FinePoint const& point : points)
        {
            auto const authority = locked->refinement.standing(point[0U], point[1U]);
            result.push_back({authority.first, authority.second});
        }
        return result;
    };
    auto const plain_base = [weak_provider](std::span<V21FinePoint const> points, bool reference) {
        auto locked = weak_provider.lock();
        if (!locked)
        {
            throw std::runtime_error{"V21 provider expired during regional geometry setup"};
        }
        std::vector<double> result;
        result.reserve(points.size());
        for (V21FinePoint const& point : points)
        {
            double const base = 0.34 * locked->plainBase(point[0U], point[1U]);
            result.push_back(reference
                ? locked->refinement.v21FixedSupportBase(base, point[0U], point[1U])
                : base);
        }
        return result;
    };

    std::vector<double> discharge(provider->water.routing.river_discharge_m3_s.size());
    std::transform(
        provider->water.routing.river_discharge_m3_s.begin(),
        provider->water.routing.river_discharge_m3_s.end(),
        discharge.begin(),
        [](float value) { return static_cast<double>(value); }
    );
    std::vector<int32_t> critical_spill_vertices;
    std::span<double const> const canonical_bed = provider->water.equilibrium.hydraulic_bed_m;
    for (V9WaterBody const& body : provider->water.equilibrium.bodies)
    {
        for (size_t index = 0U; index < canonical_bed.size(); ++index)
        {
            if (std::abs(canonical_bed[index] - body.spill_m) < 1e-9)
            {
                critical_spill_vertices.push_back(static_cast<int32_t>(index));
            }
        }
    }
    std::sort(critical_spill_vertices.begin(), critical_spill_vertices.end());
    critical_spill_vertices.erase(
        std::unique(critical_spill_vertices.begin(), critical_spill_vertices.end()),
        critical_spill_vertices.end()
    );
    V21CourseInput const course_input{
        .canonical_bed = provider->water.equilibrium.hydraulic_bed_m,
        .discharge_m3_s = discharge,
        .downstream = provider->water.routing.downstream,
        .river = provider->water.routing.river,
        .virtual_through_lake = provider->water.routing.river_virtual_through_lake,
        .reference_stage = provider->water.routing.river_stage_m,
        .critical_spill_vertices = critical_spill_vertices,
    };
    V21CourseCallbacks const course_callbacks{
        .scene_base = [plain_base](std::span<V21FinePoint const> points) { return plain_base(points, false); },
        .reference_base = [plain_base](std::span<V21FinePoint const> points) { return plain_base(points, true); },
        .standing = standing,
    };
    auto courses = buildV21CourseGeometry(course_input, course_callbacks, cancellation);
    if (!courses)
    {
        V21SurfaceQueryFailure failure;
        failure.kind = courses.error().kind == V21CourseFailure::Kind::Cancelled
            ? V21SurfaceQueryFailureKind::Cancelled
            : V21SurfaceQueryFailureKind::FinePlan;
        failure.message = "V21 course geometry failed";
        if (courses.error().source_vertex >= 0)
        {
            failure.message += " at source vertex " + std::to_string(courses.error().source_vertex);
        }
        failure.message += ": " + courses.error().message;
        return std::unexpected{std::move(failure)};
    }
    provider->scene_contacts = courses->contacts;

    V21FineGeometry geometry;
    geometry.immutable_identity = std::string{V21_MODEL_IDENTITY}
        + ";course=approved-coarse-c1;fine=approved-metric-fine-thalweg-v21;shore-contact=v1";
    geometry.chords = std::move(courses->chords);
    geometry.vertex_owner = provider->water.equilibrium.vertex_basin_owner;
    geometry.bank_fraction = 0.1;
    geometry.repose_slope = 0.65;

    V21FineCallbacks fine_callbacks;
    fine_callbacks.substrate = [weak_provider](std::span<V21FinePoint const> points) {
        auto locked = weak_provider.lock();
        if (!locked)
        {
            throw std::runtime_error{"V21 provider expired during fine profile query"};
        }
        std::vector<double> result;
        result.reserve(points.size());
        for (V21FinePoint const& point : points)
        {
            result.push_back(0.34 * locked->surface(point[0U], point[1U], 3U));
        }
        return result;
    };
    fine_callbacks.base = [weak_provider](std::span<V21FinePoint const> points) {
        auto locked = weak_provider.lock();
        if (!locked)
        {
            throw std::runtime_error{"V21 provider expired during fine profile query"};
        }
        std::vector<double> result;
        result.reserve(points.size());
        for (V21FinePoint const& point : points)
        {
            result.push_back(0.34 * locked->plainBase(point[0U], point[1U]));
        }
        return result;
    };
    fine_callbacks.standing = standing;
    fine_callbacks.protect = [weak_provider](
        std::span<double const> profile, std::span<V21FinePoint const> points
    ) {
        auto locked = weak_provider.lock();
        if (!locked || profile.size() != points.size())
        {
            throw std::runtime_error{"V21 protected profile requires a live provider and matching coordinates"};
        }
        std::vector<double> result;
        result.reserve(profile.size());
        for (size_t index = 0U; index < profile.size(); ++index)
        {
            result.push_back(0.34 * locked->protect(
                profile[index] / 0.34, points[index][0U], points[index][1U]
            ));
        }
        return result;
    };
    return std::make_unique<V21FineProvider>(std::move(geometry), std::move(fine_callbacks));
}

std::shared_ptr<GlobalAtlas const> prepareV21GlobalAtlas(
    GenerationConfiguration const& configuration,
    std::stop_token cancellation,
    ProgressCallback progress
)
{
    if (configuration.model_tag != TerrainModelTag::V21
        || (configuration.resolution != 256U && configuration.resolution != 512U))
    {
        throw std::invalid_argument{"V21 provider requires the V21 model tag and 256/512 display resolution"};
    }
    checkCancelled(cancellation);
    auto platec = generateV9PlatecSource(
        configuration.seed,
        cancellation,
        [&progress](V9PlatecProgress const update)
        {
            if (update.snapshot || update.steps_completed % 24U == 0U)
            {
                // Platec is a periodic 256-square row-major cell-centre grid at
                // 256 m spacing, mapped to source metres, not canonical H0 vertices.
                reportProgress(progress, GenerationStage::V21Platec,
                    update.steps_completed, update.step_limit,
                    update.snapshot ? makeV21PlatecSnapshot(*update.snapshot) : nullptr);
            }
        }
    );
    if (!platec)
    {
        if (platec.error() == V9PlatecFailure::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V21 Platec source exceeded its frozen native update bound"};
    }
    checkCancelled(cancellation);

    V21TerrainPreparation preparation = prepareV21Terrain(
        configuration.seed,
        *platec,
        cancellation,
        [&progress](V9TerrainStage const stage)
        {
            reportProgress(progress, GenerationStage::V21Terrain,
                stage == V9TerrainStage::Complete ? 3U : static_cast<uint32_t>(stage), 4U);
        }
    );
    checkCancelled(cancellation);

    // H0 is immutable on the periodic 256-square vertex grid at 256 m spacing,
    // in source metres (the snapshot field stores float). Publish before geomorphology.
    reportProgress(progress, GenerationStage::V21Terrain, 3U, 4U,
        makeV21TerrainSnapshot(*platec, preparation.canonical_h0_vertices_m));

    V21GeomorphologyResult geomorphology = prepareV21Geomorphology(
        preparation.canonical_h0_vertices_m,
        cancellation
    );
    V9TerrainResult terrain = std::move(preparation.native);
    terrain.final_bed_vertices_m = std::move(geomorphology.source_proxy_bed_m);
    reportProgress(
        progress,
        GenerationStage::V21Terrain,
        4U,
        4U,
        makeV21TerrainSnapshot(*platec, terrain.final_bed_vertices_m)
    );

    constexpr uint32_t CLIMATE_WORK_TOTAL = 1U + 180U + 1'080U;
    std::vector<double> climate_equivalent_bed(NATIVE_CELL_COUNT);
    for (size_t index = 0U; index < climate_equivalent_bed.size(); ++index)
    {
        if ((index & 255U) == 0U)
        {
            checkCancelled(cancellation);
        }
        climate_equivalent_bed[index] = 3.4 * terrain.final_bed_vertices_m[index];
        if (!std::isfinite(climate_equivalent_bed[index]))
        {
            throw std::runtime_error{"V21 climate-equivalent terrain mapping is nonfinite"};
        }
    }
    auto climate = generateV9Climate(
        climate_equivalent_bed,
        configuration.seed,
        cancellation,
        [&progress](V9ClimateProgress const update)
        {
            reportProgress(progress, GenerationStage::V21Climate,
                climateProgressPosition(update), CLIMATE_WORK_TOTAL);
        }
    );
    if (!climate)
    {
        if (climate.error() == V9ClimateFailure::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V21 native climate stage failed its source numerical gates"};
    }
    checkCancelled(cancellation);
    reportProgress(
        progress,
        GenerationStage::V21Climate,
        CLIMATE_WORK_TOTAL,
        CLIMATE_WORK_TOTAL,
        makeV21ClimateSnapshot(*platec, terrain.final_bed_vertices_m, *climate)
    );

    V9WaterForcing forcing = makeWaterForcing(*climate);
    forcing.climate_equivalent_metres_per_bed_unit = 10.0;
    std::vector<double> water_bed_game_blocks(NATIVE_CELL_COUNT);
    for (size_t index = 0; index < water_bed_game_blocks.size(); ++index)
    {
        water_bed_game_blocks[index] = 0.34 * terrain.final_bed_vertices_m[index];
    }
    auto water = generateV9Water(
        water_bed_game_blocks,
        forcing,
        cancellation,
        [&progress](V9WaterProgress const update)
        {
            uint32_t completed = 0U;
            switch (update.phase)
            {
            case V9WaterPhase::Routing:
                completed = 0U;
                break;
            case V9WaterPhase::MarineSalt:
                completed = 1U;
                break;
            case V9WaterPhase::SnowAndIce:
                completed = 2U + std::min(update.completed_items, 8U);
                break;
            case V9WaterPhase::Complete:
                completed = 10U;
                break;
            }
            reportProgress(progress, GenerationStage::V21Water, completed, 12U);
        }
    );
    if (!water)
    {
        if (water.error().kind == V9WaterFailure::Kind::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V21 water/ice stage failed: " + water.error().message};
    }
    for (size_t index = 0U; index < NATIVE_CELL_COUNT; ++index)
    {
        if (water->routing.river[index] != 0U
            && water->equilibrium.ocean[index] == 0U
            && water->equilibrium.lake[index] == 0U)
        {
            water->land_inventory.biome_id[index] = 12U;
        }
    }
    checkCancelled(cancellation);
    reportProgress(
        progress,
        GenerationStage::V21Water,
        10U,
        12U,
        makeV21WaterSnapshot(*platec, terrain.final_bed_vertices_m, *water, *climate)
    );

    auto refinement = V9WaterRefinement::create(water->equilibrium, water->routing);
    if (!refinement)
    {
        throw std::runtime_error{"V21 protected water-contact setup failed: " + refinement.error().message};
    }

    auto provider = std::make_shared<V21ProviderState>(
        std::move(*platec),
        std::move(terrain),
        std::move(preparation.canonical_h0_vertices_m),
        std::move(geomorphology.evolved_bed_m),
        std::move(geomorphology.plains),
        std::move(*climate),
        std::move(*water),
        std::move(*refinement)
    );
    V9WaterQueryModel const query_model{
        .base = [provider](double x_m, double y_m) {
            return provider->refinement.v21FixedSupportBase(
                0.34 * provider->plainBase(x_m, y_m), x_m, y_m
            );
        },
        .reaches = provider->refinement.reaches(),
        .contacts = provider->refinement.protectedContacts(),
        .progress = [&cancellation](char const*, uint64_t, uint64_t) { return !cancellation.stop_requested(); },
    };
    auto fixed_support = buildV9WaterFixedSupport(provider->water.equilibrium, query_model);
    if (!fixed_support)
    {
        if (fixed_support.error().kind == V9WaterFailure::Kind::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V21 fixed local-water support failed: " + fixed_support.error().message};
    }
    std::span<V9RefinementReach const> const v21_reaches = provider->refinement.reaches();
    fixed_support->river_reach_order.resize(v21_reaches.size());
    std::iota(fixed_support->river_reach_order.begin(), fixed_support->river_reach_order.end(), size_t{0});
    std::stable_sort(
        fixed_support->river_reach_order.begin(),
        fixed_support->river_reach_order.end(),
        [&v21_reaches](size_t a, size_t b) {
            return v21_reaches[a].discharge_m3_s > v21_reaches[b].discharge_m3_s;
        }
    );
    provider->fixed_water_support = std::move(*fixed_support);
    reportProgress(progress, GenerationStage::V21Water, 11U, 12U);

    auto fine_provider = makeV21FineProvider(provider, cancellation);
    if (!fine_provider)
    {
        if (fine_provider.error().kind == V21SurfaceQueryFailureKind::Cancelled)
        {
            throw GenerationCancelled{};
        }
        provider->fine_provider_failure = std::move(fine_provider.error());
    }
    else
    {
        provider->fine_provider = std::move(*fine_provider);
    }
    std::shared_ptr<GlobalAtlas const> atlas = makeV21Atlas(configuration, std::move(provider));
    static_cast<void>(makeRegisteredFields(*atlas->v21_provider));
    verifyV21MappedDomain(*atlas, cancellation);
    checkCancelled(cancellation);
    reportProgress(progress, GenerationStage::V21Water, 12U, 12U);
    reportProgress(progress, GenerationStage::Complete, 1U, 1U);
    return atlas;
}

std::shared_ptr<GlobalAtlas const> prepareV9GlobalAtlas(
    GenerationConfiguration const& configuration,
    std::stop_token cancellation,
    ProgressCallback progress
)
{
    if (configuration.model_tag != TerrainModelTag::V9
        || (configuration.resolution != 256U && configuration.resolution != 512U))
    {
        throw std::invalid_argument{"V9 provider requires the V9 model tag and 256/512 display resolution"};
    }
    checkCancelled(cancellation);

    auto platec = generateV9PlatecSource(
        configuration.seed,
        cancellation,
        [&progress](V9PlatecProgress const update)
        {
            if (update.steps_completed == 0U || update.complete || update.steps_completed % 24U == 0U)
            {
                reportProgress(progress, GenerationStage::V9Platec, update.steps_completed, update.step_limit);
            }
        }
    );
    if (!platec)
    {
        if (platec.error() == V9PlatecFailure::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V9 Platec source exceeded its frozen native update bound"};
    }
    checkCancelled(cancellation);

    reportProgress(
        progress,
        GenerationStage::V9Platec,
        static_cast<uint32_t>(platec->final_step),
        V9_PLATEC_SOURCE_STEP_LIMIT,
        makeV9PlatecSnapshot(*platec)
    );

    uint32_t terrain_work = 0U;
    V9TerrainResult terrain = prepareV9Terrain(
        configuration.seed,
        *platec,
        cancellation,
        [&progress, &terrain_work](V9TerrainStage)
        {
            ++terrain_work;
            reportProgress(progress, GenerationStage::V9Terrain, terrain_work, 6U);
        }
    );
    checkCancelled(cancellation);
    auto terrain_snapshot = std::make_shared<GenerationSnapshot>();
    terrain_snapshot->stage = GenerationStage::V9Terrain;
    terrain_snapshot->resolution = NATIVE_RESOLUTION;
    terrain_snapshot->v9 = makeV9SnapshotFields(*platec, terrain.final_bed_vertices_m);
    reportProgress(progress, GenerationStage::V9Terrain, terrain_work, 6U, std::move(terrain_snapshot));

    constexpr uint32_t CLIMATE_WORK_TOTAL = 1U + 180U + 1'080U;
    auto climate = generateV9Climate(
        terrain.final_bed_vertices_m,
        configuration.seed,
        cancellation,
        [&progress](V9ClimateProgress const update)
        {
            reportProgress(
                progress,
                GenerationStage::V9Climate,
                climateProgressPosition(update),
                CLIMATE_WORK_TOTAL
            );
        }
    );
    if (!climate)
    {
        if (climate.error() == V9ClimateFailure::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V9 native climate stage failed its source numerical gates"};
    }
    checkCancelled(cancellation);
    reportProgress(
        progress,
        GenerationStage::V9Climate,
        CLIMATE_WORK_TOTAL,
        CLIMATE_WORK_TOTAL,
        makeV9ClimateSnapshot(*platec, terrain, *climate)
    );

    V9WaterForcing const forcing = makeWaterForcing(*climate);
    auto water = generateV9Water(
        terrain.final_bed_vertices_m,
        forcing,
        cancellation,
        [&progress](V9WaterProgress const update)
        {
            uint32_t completed = 0U;
            switch (update.phase)
            {
            case V9WaterPhase::Routing:
                completed = 0U;
                break;
            case V9WaterPhase::MarineSalt:
                completed = 1U;
                break;
            case V9WaterPhase::SnowAndIce:
                completed = 2U + std::min(update.completed_items, 8U);
                break;
            case V9WaterPhase::Complete:
                completed = 10U;
                break;
            }
            reportProgress(progress, GenerationStage::V9Water, completed, 10U);
        }
    );
    if (!water)
    {
        if (water.error().kind == V9WaterFailure::Kind::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V9 water/ice stage failed: " + water.error().message};
    }
    checkCancelled(cancellation);

    reportProgress(
        progress,
        GenerationStage::V9Water,
        10U,
        10U,
        makeV9WaterSnapshot(*platec, terrain, *water, *climate, GenerationStage::V9Water)
    );

    auto refinement = V9WaterRefinement::create(water->equilibrium, water->routing);
    if (!refinement)
    {
        throw std::runtime_error{"V9 water-conditioned structural query setup failed: " + refinement.error().message};
    }

    auto provider = std::make_shared<V9ProviderState>(
        std::move(*platec),
        std::move(terrain),
        std::move(*climate),
        std::move(*water),
        std::move(*refinement)
    );
    V9WaterQueryModel const query_model = makeV9WaterQueryModel(provider->refinement);
    auto fixed_support = buildV9WaterFixedSupport(provider->water.equilibrium, query_model);
    if (!fixed_support)
    {
        if (fixed_support.error().kind == V9WaterFailure::Kind::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error{"V9 fixed water-query support failed: " + fixed_support.error().message};
    }
    provider->fixed_water_support = std::move(*fixed_support);

    std::shared_ptr<GlobalAtlas const> atlas = makeAtlas(configuration, std::move(provider));
    static_cast<void>(v9RegisteredFields(*atlas));
    verifyMappedDomain(*atlas, cancellation);
    checkCancelled(cancellation);
    reportProgress(progress, GenerationStage::Complete, 1U, 1U);
    return atlas;
}

} // namespace generation::detail

namespace generation
{
namespace
{

[[nodiscard]]
detail::V9ProviderState const& requireProvider(GlobalAtlas const& atlas)
{
    if (atlas.configuration.model_tag != TerrainModelTag::V9 || !atlas.v9_provider)
    {
        throw std::invalid_argument{"operation requires an immutable V9 provider atlas"};
    }
    return *atlas.v9_provider;
}

[[nodiscard]]
detail::V21ProviderState const& requireV21Provider(GlobalAtlas const& atlas)
{
    if (atlas.configuration.model_tag != TerrainModelTag::V21 || !atlas.v21_provider)
    {
        throw std::invalid_argument{"operation requires an immutable V21 provider atlas"};
    }
    return *atlas.v21_provider;
}

[[nodiscard]]
double periodic(double coordinate, double period)
{
    double wrapped = std::fmod(coordinate, period);
    if (wrapped < 0.0)
    {
        wrapped += period;
    }
    return wrapped;
}

constexpr uint32_t NATIVE_RESOLUTION = detail::NATIVE_RESOLUTION;
constexpr uint32_t NATIVE_CELL_COUNT = detail::NATIVE_CELL_COUNT;
constexpr uint32_t PHASE_COUNT = detail::PHASE_COUNT;
constexpr double WORLD_METRES = detail::WORLD_METRES;

struct GridCoordinate final
{
    uint32_t x0;
    uint32_t x1;
    uint32_t y0;
    uint32_t y1;
    double fx;
    double fy;
};

[[nodiscard]]
GridCoordinate coordinate(double world_x_m, double world_y_m)
{
    double const x = periodic(world_x_m, WORLD_METRES) / (WORLD_METRES / NATIVE_RESOLUTION);
    double const y = periodic(world_y_m, WORLD_METRES) / (WORLD_METRES / NATIVE_RESOLUTION);
    double const x_floor = std::floor(x);
    double const y_floor = std::floor(y);
    uint32_t const x0 = static_cast<uint32_t>(x_floor);
    uint32_t const y0 = static_cast<uint32_t>(y_floor);
    return {
        .x0 = x0,
        .x1 = (x0 + 1U) % NATIVE_RESOLUTION,
        .y0 = y0,
        .y1 = (y0 + 1U) % NATIVE_RESOLUTION,
        .fx = x - x_floor,
        .fy = y - y_floor,
    };
}

[[nodiscard]]
double bilinear(std::span<float const> values, GridCoordinate const& at)
{
    auto const value = [&values](uint32_t x, uint32_t y)
    {
        return static_cast<double>(values[static_cast<size_t>(y) * NATIVE_RESOLUTION + x]);
    };
    double const south = value(at.x0, at.y0) * (1.0 - at.fx) + value(at.x1, at.y0) * at.fx;
    double const north = value(at.x0, at.y1) * (1.0 - at.fx) + value(at.x1, at.y1) * at.fx;
    return south * (1.0 - at.fy) + north * at.fy;
}

[[nodiscard]]
double bilinear(std::span<double const> values, GridCoordinate const& at)
{
    auto const value = [&values](uint32_t x, uint32_t y)
    {
        return values[static_cast<size_t>(y) * NATIVE_RESOLUTION + x];
    };
    double const south = value(at.x0, at.y0) * (1.0 - at.fx) + value(at.x1, at.y0) * at.fx;
    double const north = value(at.x0, at.y1) * (1.0 - at.fx) + value(at.x1, at.y1) * at.fx;
    return south * (1.0 - at.fy) + north * at.fy;
}

[[nodiscard]]
double meanPhaseField(
    std::span<float const> values,
    GridCoordinate const& at
)
{
    size_t const phase_size = static_cast<size_t>(NATIVE_CELL_COUNT);
    double mean = 0.0;
    for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
    {
        mean += bilinear(values.subspan(static_cast<size_t>(phase) * phase_size, phase_size), at) / PHASE_COUNT;
    }
    return mean;
}

} // namespace

std::vector<V9RegisteredFieldView> v9RegisteredFields(GlobalAtlas const& atlas)
{
    return detail::makeRegisteredFields(requireProvider(atlas));
}

std::vector<V9RegisteredFieldView> v21RegisteredFields(GlobalAtlas const& atlas)
{
    return detail::makeRegisteredFields(requireV21Provider(atlas));
}

std::span<uint32_t const> v21CurrentPlateIds(GlobalAtlas const& atlas)
{
    return requireV21Provider(atlas).platec.current_plate_id;
}

std::span<V9PlateDirection const> v21PlateDirections(GlobalAtlas const& atlas)
{
    return requireV21Provider(atlas).public_plate_directions;
}

double v21L0ElevationAt(GlobalAtlas const& atlas, double const world_x_m, double const world_y_m)
{
    if (!std::isfinite(world_x_m) || !std::isfinite(world_y_m))
    {
        throw std::invalid_argument{"V21 L0 elevation query requires finite source metres"};
    }
    detail::V21ProviderState const& provider = requireV21Provider(atlas);
    auto wrap = [](double coordinate) {
        double wrapped = std::fmod(coordinate, WORLD_METRES);
        return wrapped < 0.0 ? wrapped + WORLD_METRES : wrapped;
    };
    double const wrapped_x = wrap(world_x_m);
    double const wrapped_y = wrap(world_y_m);
    uint32_t const vertex_x = static_cast<uint32_t>(std::llround(wrapped_x / 256.0));
    uint32_t const vertex_y = static_cast<uint32_t>(std::llround(wrapped_y / 256.0));
    if (vertex_x < NATIVE_RESOLUTION && vertex_y < NATIVE_RESOLUTION
        && wrapped_x == static_cast<double>(vertex_x) * 256.0
        && wrapped_y == static_cast<double>(vertex_y) * 256.0)
    {
        return provider.terrain.final_bed_vertices_m[
            static_cast<size_t>(vertex_y) * NATIVE_RESOLUTION + vertex_x];
    }
    return provider.surface(world_x_m, world_y_m, 0U);
}

std::span<uint32_t const> v9CurrentPlateIds(GlobalAtlas const& atlas)
{
    return requireProvider(atlas).platec.current_plate_id;
}

std::span<V9PlateDirection const> v9PlateDirections(GlobalAtlas const& atlas)
{
    auto const& directions = requireProvider(atlas).public_plate_directions;
    return directions;
}

V9SurfaceSample sampleV9Surface(GlobalAtlas const& atlas, double world_x_m, double world_y_m, uint32_t lod)
{
    if (!std::isfinite(world_x_m) || !std::isfinite(world_y_m) || lod > 3U)
    {
        throw std::invalid_argument{"V9 terrain query requires finite source metres and LOD 0..3"};
    }
    detail::V9ProviderState const& provider = requireProvider(atlas);
    GridCoordinate const at = coordinate(world_x_m, world_y_m);
    uint32_t const nearest_x = at.fx < 0.5 ? at.x0 : at.x1;
    uint32_t const nearest_y = at.fy < 0.5 ? at.y0 : at.y1;
    size_t const nearest = static_cast<size_t>(nearest_y) * NATIVE_RESOLUTION + nearest_x;
    double const level = bilinear(provider.water.water_level_m, at);
    double const temperature = meanPhaseField(provider.water.temperature_C, at);
    double const precipitation = meanPhaseField(provider.water.precipitation_mm_year, at);
    return {
        .elevation_m = provider.surface(world_x_m, world_y_m, lod),
        .water_level_m = level,
        .temperature_celsius = temperature,
        .precipitation_mm_year = precipitation,
        .biome_id = provider.water.land_inventory.biome_id[nearest],
        .ocean = provider.water.equilibrium.ocean[nearest] != 0U,
        .lake = provider.water.equilibrium.lake[nearest] != 0U,
        .river = provider.water.routing.river[nearest] != 0U,
    };
}

std::expected<V21SurfaceQueryResult, V21SurfaceQueryFailure> queryV21SurfaceGrid(
    GlobalAtlas const& atlas,
    V21SurfaceQueryGrid const& grid,
    std::stop_token cancellation
)
{
    auto invalid = [](std::string message) {
        return std::unexpected{V21SurfaceQueryFailure{
            V21SurfaceQueryFailureKind::InvalidInput, std::move(message), {}}};
    };
    if (atlas.configuration.model_tag != TerrainModelTag::V21 || !atlas.v21_provider)
    {
        return invalid("V21 regional query requires a V21 provider atlas");
    }
    uint64_t const point_count = static_cast<uint64_t>(grid.rows) * grid.columns;
    double const extent_x = grid.columns > 0U ? static_cast<double>(grid.columns - 1U) * grid.spacing_m : 0.0;
    double const extent_y = grid.rows > 0U ? static_cast<double>(grid.rows - 1U) * grid.spacing_m : 0.0;
    if (grid.rows == 0U || grid.columns == 0U || point_count > 1'048'576U
        || grid.lod > 3U || grid.selected_phase >= PHASE_COUNT
        || !std::isfinite(grid.origin_x_m) || !std::isfinite(grid.origin_y_m)
        || !std::isfinite(grid.spacing_m) || grid.spacing_m <= 0.0
        || !std::isfinite(extent_x) || !std::isfinite(extent_y)
        || !std::isfinite(grid.origin_x_m + extent_x) || !std::isfinite(grid.origin_y_m + extent_y))
    {
        return invalid("V21 regional query requires a finite nonempty grid, lod0..3, phase0..7, and at most1048576 points");
    }
    if (cancellation.stop_requested())
    {
        return std::unexpected{V21SurfaceQueryFailure{
            V21SurfaceQueryFailureKind::Cancelled, "V21 regional query cancelled", {}}};
    }
    detail::V21ProviderState const& provider = *atlas.v21_provider;
    if (provider.fine_provider_failure)
    {
        return std::unexpected{*provider.fine_provider_failure};
    }
    if (!provider.fine_provider)
    {
        return invalid("V21 immutable course/fine provider is unavailable");
    }
    size_t const count = static_cast<size_t>(point_count);
    std::vector<double> x(count), y(count), background(count);
    for (uint32_t row = 0U; row < grid.rows; ++row)
    {
        if (cancellation.stop_requested())
        {
            return std::unexpected{V21SurfaceQueryFailure{
                V21SurfaceQueryFailureKind::Cancelled, "V21 background sampling cancelled", {}}};
        }
        for (uint32_t column = 0U; column < grid.columns; ++column)
        {
            size_t const index = static_cast<size_t>(row) * grid.columns + column;
            x[index] = grid.origin_x_m + static_cast<double>(column) * grid.spacing_m;
            y[index] = grid.origin_y_m + static_cast<double>(row) * grid.spacing_m;
            background[index] = 0.34 * provider.surface(x[index], y[index], grid.lod);
        }
    }
    detail::V21FineGrid const fine_grid{
        .origin_x_m = grid.origin_x_m,
        .origin_y_m = grid.origin_y_m,
        .spacing_m = grid.spacing_m,
        .rows = grid.rows,
        .columns = grid.columns,
    };
    auto plan = provider.fine_provider->forRegion(fine_grid, grid.lod, cancellation);
    if (!plan)
    {
        V21SurfaceQueryFailure failure;
        failure.kind = plan.error().kind == detail::V21FineFailureKind::Cancelled
            ? V21SurfaceQueryFailureKind::Cancelled
            : V21SurfaceQueryFailureKind::FinePlan;
        failure.message = plan.error().reason;
        failure.witnesses.reserve(plan.error().witnesses.size());
        for (detail::V21FineWitness const& witness : plan.error().witnesses)
        {
            failure.witnesses.push_back({
                .edge = witness.edge,
                .node = witness.node,
                .source_vertex = witness.source_vertex,
                .side = witness.side,
                .required_stage_blocks = witness.required_stage,
                .maximum_cut_only_stage_blocks = witness.maximum_cut_only_stage,
                .deficit_blocks = witness.deficit,
                .xy0_m = witness.xy0,
                .xy1_m = witness.xy1,
            });
        }
        return std::unexpected{std::move(failure)};
    }
    auto profiled = (*plan)->applyProfile(background, x, y, grid.lod, cancellation);
    if (!profiled)
    {
        V21SurfaceQueryFailure failure;
        failure.kind = profiled.error().kind == detail::V21FineFailureKind::Cancelled
            ? V21SurfaceQueryFailureKind::Cancelled
            : V21SurfaceQueryFailureKind::FinePlan;
        failure.message = profiled.error().reason;
        failure.witnesses.reserve(profiled.error().witnesses.size());
        for (detail::V21FineWitness const& witness : profiled.error().witnesses)
        {
            failure.witnesses.push_back({
                .edge = witness.edge,
                .node = witness.node,
                .source_vertex = witness.source_vertex,
                .side = witness.side,
                .required_stage_blocks = witness.required_stage,
                .maximum_cut_only_stage_blocks = witness.maximum_cut_only_stage,
                .deficit_blocks = witness.deficit,
                .xy0_m = witness.xy0,
                .xy1_m = witness.xy1,
            });
        }
        return std::unexpected{std::move(failure)};
    }

    if (provider.water.land_inventory.snow_water_equivalent_m.size() != PHASE_COUNT * NATIVE_CELL_COUNT
        || provider.water.land_inventory.glacier_thickness_m.size() != PHASE_COUNT * NATIVE_CELL_COUNT
        || provider.water.sea_ice_thickness_m.size() != PHASE_COUNT * NATIVE_CELL_COUNT)
    {
        return invalid("V21 phase-resolved ice fields do not match the pinned 8x256-square contract");
    }
    std::vector<detail::V9RefinementReach> reaches;
    reaches.reserve((*plan)->chords().size());
    for (detail::V21FineChord const& chord : (*plan)->chords())
    {
        reaches.push_back({
            .start = chord.start,
            .target = chord.target,
            .a = chord.a,
            .delta = chord.delta,
            .length2 = chord.delta[0U] * chord.delta[0U] + chord.delta[1U] * chord.delta[1U],
            .grade0 = chord.grade0,
            .grade1 = chord.grade1,
            .water_stage0 = chord.water_stage0,
            .water_stage1 = chord.water_stage1,
            .halfwidth = chord.halfwidth,
            .support = chord.support_m,
            .discharge_m3_s = chord.discharge_m3_s,
            .shore_truncated = chord.shore_truncated,
        });
    }
    detail::V9WaterQueryModel const water_model{
        .base = {},
        .reaches = reaches,
        .contacts = {},
        .progress = [&cancellation](char const*, uint64_t, uint64_t) {
            return !cancellation.stop_requested();
        },
    };
    detail::V9WaterQueryGrid const water_grid{
        .rows = grid.rows,
        .columns = grid.columns,
        .origin_x_m = grid.origin_x_m,
        .origin_y_m = grid.origin_y_m,
        .spacing_m = grid.spacing_m,
        .bed_m = *profiled,
    };
    auto water = detail::queryV21WaterLocalSupport(
        provider.water.equilibrium, provider.fixed_water_support, water_grid, water_model
    );
    if (!water)
    {
        return std::unexpected{V21SurfaceQueryFailure{
            water.error().kind == detail::V9WaterFailure::Kind::Cancelled
                ? V21SurfaceQueryFailureKind::Cancelled
                : V21SurfaceQueryFailureKind::Water,
            water.error().message,
            {}}};
    }

    detail::V9WaterLandClimate local_climate;
    local_climate.phase_count = PHASE_COUNT;
    local_climate.temperature_C.resize(static_cast<size_t>(PHASE_COUNT) * count);
    local_climate.precipitation_mm_year.resize(static_cast<size_t>(PHASE_COUNT) * count);
    std::vector<double> local_snow(static_cast<size_t>(PHASE_COUNT) * count);
    std::vector<double> local_glacier(static_cast<size_t>(PHASE_COUNT) * count);
    std::vector<double> mean_temperature(count, 0.0), mean_precipitation(count, 0.0);
    for (size_t index = 0U; index < count; ++index)
    {
        GridCoordinate const at = coordinate(x[index], y[index]);
        double const local_source_height = (*profiled)[index] / 0.34;
        double const climate_equivalent_height = std::max(3.4 * local_source_height, 0.0);
        double const canonical_source_height = bilinear(
            std::span<double const>{provider.terrain.final_bed_vertices_m}, at
        );
        double const canonical_equivalent_height = std::max(3.4 * canonical_source_height, 0.0);
        double const local_lapse = 0.0065 * (climate_equivalent_height - canonical_equivalent_height);
        for (uint32_t phase = 0U; phase < PHASE_COUNT; ++phase)
        {
            size_t const phase_offset = static_cast<size_t>(phase) * NATIVE_CELL_COUNT;
            size_t const local_offset = static_cast<size_t>(phase) * count + index;
            double const temperature = bilinear(
                std::span<float const>{provider.water.temperature_C}.subspan(phase_offset, NATIVE_CELL_COUNT), at
            ) - local_lapse;
            double const precipitation = bilinear(
                std::span<float const>{provider.water.precipitation_mm_year}.subspan(phase_offset, NATIVE_CELL_COUNT), at
            );
            local_climate.temperature_C[local_offset] = temperature;
            local_climate.precipitation_mm_year[local_offset] = precipitation;
            local_snow[local_offset] = bilinear(
                std::span<float const>{provider.water.land_inventory.snow_water_equivalent_m}
                    .subspan(phase_offset, NATIVE_CELL_COUNT), at
            );
            local_glacier[local_offset] = bilinear(
                std::span<float const>{provider.water.land_inventory.glacier_thickness_m}
                    .subspan(phase_offset, NATIVE_CELL_COUNT), at
            );
            mean_temperature[index] += temperature / static_cast<double>(PHASE_COUNT);
            mean_precipitation[index] += precipitation / static_cast<double>(PHASE_COUNT);
        }
    }
    auto local_classification = detail::classifyV9WaterLocalBiomes(
        local_climate, local_snow, local_glacier, water->ocean, water->lake, water->river
    );
    if (!local_classification)
    {
        return std::unexpected{V21SurfaceQueryFailure{
            V21SurfaceQueryFailureKind::Water, local_classification.error().message, {}}};
    }

    V21SurfaceQueryResult result;
    result.rows = grid.rows;
    result.columns = grid.columns;
    result.samples.resize(count);
    size_t const selected_phase_offset = static_cast<size_t>(grid.selected_phase) * count;
    size_t const native_phase_offset = static_cast<size_t>(grid.selected_phase) * NATIVE_CELL_COUNT;
    for (size_t index = 0U; index < count; ++index)
    {
        if (cancellation.stop_requested())
        {
            return std::unexpected{V21SurfaceQueryFailure{
                V21SurfaceQueryFailureKind::Cancelled, "V21 result materialization cancelled", {}}};
        }
        try
        {
            static_cast<void>(mapV21ElevationToBlock((*profiled)[index] / 0.34));
            static_cast<void>(mapV21WaterLevelToBlock(water->water_level_m[index] / 0.34));
        }
        catch (std::exception const& error)
        {
            return std::unexpected{V21SurfaceQueryFailure{
                V21SurfaceQueryFailureKind::InvalidInput,
                std::string{"V21 surface exceeds the declared block-height domain: "} + error.what(),
                {}}};
        }
        GridCoordinate const at = coordinate(x[index], y[index]);
        double const sea_ice = water->ocean[index] != 0U
            ? bilinear(std::span<float const>{provider.water.sea_ice_thickness_m}
                .subspan(native_phase_offset, NATIVE_CELL_COUNT), at)
            : 0.0;
        result.samples[index] = {
            .elevation_m = (*profiled)[index] / 0.34,
            .water_level_m = water->water_level_m[index] / 0.34,
            .temperature_celsius = mean_temperature[index],
            .precipitation_mm_year = mean_precipitation[index],
            .biome_id = local_classification->biome_id[index],
            .ocean = water->ocean[index] != 0U,
            .lake = water->lake[index] != 0U,
            .river = water->river[index] != 0U,
            .land_snow_water_equivalent_m = local_classification->snow_water_equivalent_m
                [selected_phase_offset + index],
            .glacier_thickness_m = local_classification->glacier_thickness_m
                [selected_phase_offset + index],
            .sea_ice_thickness_m = sea_ice,
        };
    }
    return result;
}

V9SurfaceSample sampleV21Surface(
    GlobalAtlas const& atlas,
    double world_x_m,
    double world_y_m,
    uint32_t lod,
    uint32_t selected_phase
)
{
    V21SurfaceQueryGrid const grid{
        .rows = 1U,
        .columns = 1U,
        .origin_x_m = world_x_m,
        .origin_y_m = world_y_m,
        .spacing_m = 1.0,
        .lod = lod,
        .selected_phase = selected_phase,
    };
    auto result = queryV21SurfaceGrid(atlas, grid);
    if (!result)
    {
        throw std::runtime_error{"V21 surface query failed: " + result.error().message};
    }
    return result->samples.front();
}

bool v9WaterCoversIntegerBlockCell(
    GlobalAtlas const& atlas,
    uint32_t const origin_x,
    uint32_t const origin_y,
    uint32_t const extent
) noexcept
{
    if (atlas.configuration.model_tag != TerrainModelTag::V9 || !atlas.v9_provider
        || (atlas.configuration.resolution != 256U && atlas.configuration.resolution != 512U)
        || generation::WORLD_SIZE_BLOCKS % atlas.configuration.resolution != 0U
        || extent != generation::WORLD_SIZE_BLOCKS / atlas.configuration.resolution
        || origin_x >= generation::WORLD_SIZE_BLOCKS || origin_y >= generation::WORLD_SIZE_BLOCKS
        || origin_x % extent != 0U || origin_y % extent != 0U
        || static_cast<uint64_t>(origin_x) + extent > generation::WORLD_SIZE_BLOCKS
        || static_cast<uint64_t>(origin_y) + extent > generation::WORLD_SIZE_BLOCKS)
    {
        return false;
    }

    detail::V9ProviderState const& provider = *atlas.v9_provider;
    auto const& water = provider.water.equilibrium;
    auto const& river = provider.water.routing.river;
    if (water.resolution != NATIVE_RESOLUTION || water.ocean.size() != NATIVE_CELL_COUNT
        || water.lake.size() != NATIVE_CELL_COUNT || river.size() != NATIVE_CELL_COUNT
        || generation::WORLD_SIZE_BLOCKS % NATIVE_RESOLUTION != 0U)
    {
        return false;
    }

    constexpr uint32_t source_cell_extent = generation::WORLD_SIZE_BLOCKS / NATIVE_RESOLUTION;
    auto const nearest_source_cell = [](uint32_t const coordinate) {
        uint32_t const cell = coordinate / source_cell_extent;
        uint32_t const offset = coordinate % source_cell_extent;
        return (cell + static_cast<uint32_t>(offset * 2U >= source_cell_extent)) % NATIVE_RESOLUTION;
    };
    std::array<uint32_t, 2U> const xs{
        nearest_source_cell(origin_x),
        nearest_source_cell(origin_x + extent - 1U),
    };
    std::array<uint32_t, 2U> const ys{
        nearest_source_cell(origin_y),
        nearest_source_cell(origin_y + extent - 1U),
    };
    for (uint32_t const y : ys)
    {
        for (uint32_t const x : xs)
        {
            size_t const index = static_cast<size_t>(y) * NATIVE_RESOLUTION + x;
            if (water.ocean[index] == 0U && water.lake[index] == 0U && river[index] == 0U)
            {
                return false;
            }
        }
    }
    return true;
}

bool v21WaterCoversIntegerBlockCell(
    GlobalAtlas const& atlas,
    uint32_t const origin_x,
    uint32_t const origin_y,
    uint32_t const extent
) noexcept
{
    if (atlas.configuration.model_tag != TerrainModelTag::V21 || !atlas.v21_provider
        || (atlas.configuration.resolution != 256U && atlas.configuration.resolution != 512U)
        || WORLD_SIZE_BLOCKS % atlas.configuration.resolution != 0U
        || extent != WORLD_SIZE_BLOCKS / atlas.configuration.resolution
        || origin_x >= WORLD_SIZE_BLOCKS || origin_y >= WORLD_SIZE_BLOCKS
        || origin_x % extent != 0U || origin_y % extent != 0U
        || static_cast<uint64_t>(origin_x) + extent > WORLD_SIZE_BLOCKS
        || static_cast<uint64_t>(origin_y) + extent > WORLD_SIZE_BLOCKS)
    {
        return false;
    }
    detail::V21ProviderState const& provider = *atlas.v21_provider;
    auto const& water = provider.water.equilibrium;
    auto const& river = provider.water.routing.river;
    if (water.resolution != NATIVE_RESOLUTION || water.ocean.size() != NATIVE_CELL_COUNT
        || water.lake.size() != NATIVE_CELL_COUNT || river.size() != NATIVE_CELL_COUNT
        || WORLD_SIZE_BLOCKS % NATIVE_RESOLUTION != 0U)
    {
        return false;
    }
    constexpr uint32_t SOURCE_CELL_EXTENT = WORLD_SIZE_BLOCKS / NATIVE_RESOLUTION;
    auto const nearest_source_cell = [](uint32_t const coordinate) {
        uint32_t const cell = coordinate / SOURCE_CELL_EXTENT;
        uint32_t const offset = coordinate % SOURCE_CELL_EXTENT;
        return (cell + static_cast<uint32_t>(offset * 2U >= SOURCE_CELL_EXTENT)) % NATIVE_RESOLUTION;
    };
    std::array<uint32_t, 2U> const xs{
        nearest_source_cell(origin_x),
        nearest_source_cell(origin_x + extent - 1U),
    };
    std::array<uint32_t, 2U> const ys{
        nearest_source_cell(origin_y),
        nearest_source_cell(origin_y + extent - 1U),
    };
    for (uint32_t const y : ys)
    {
        for (uint32_t const x : xs)
        {
            size_t const index = static_cast<size_t>(y) * NATIVE_RESOLUTION + x;
            if (water.ocean[index] == 0U && water.lake[index] == 0U && river[index] == 0U)
            {
                return false;
            }
        }
    }
    return true;
}

std::expected<V9WaterGridResult, V9WaterQueryFailure> queryV9WaterGrid(
    GlobalAtlas const& atlas,
    V9WaterGrid const& grid,
    std::stop_token cancellation
)
{
    detail::V9ProviderState const& provider = requireProvider(atlas);
    detail::V9WaterQueryGrid const query_grid{
        .rows = grid.rows,
        .columns = grid.columns,
        .origin_x_m = grid.origin_x_m,
        .origin_y_m = grid.origin_y_m,
        .spacing_m = grid.spacing_m,
        .bed_m = grid.bed_m,
    };
    detail::V9WaterQueryModel model = detail::makeV9WaterQueryModel(provider.refinement);
    model.progress = [&cancellation](char const*, uint64_t, uint64_t)
    {
        return !cancellation.stop_requested();
    };
    auto support = detail::queryV9WaterLocalSupport(
        provider.water.equilibrium,
        provider.fixed_water_support,
        query_grid,
        model
    );
    if (!support)
    {
        return std::unexpected(V9WaterQueryFailure{
            .message = support.error().message,
            .cancelled = support.error().kind == detail::V9WaterFailure::Kind::Cancelled,
        });
    }
    return V9WaterGridResult{
        .rows = grid.rows,
        .columns = grid.columns,
        .ocean = std::move(support->ocean),
        .lake = std::move(support->lake),
        .river = std::move(support->river),
        .lake_id = std::move(support->lake_id),
        .water_level_m = std::move(support->water_level_m),
        .water_depth_m = std::move(support->water_depth_m),
    };
}

uint16_t mapV9ElevationToBlock(double source_elevation_m)
{
    double const mapped = std::floor(384.0 + source_elevation_m / 10.0);
    if (!std::isfinite(source_elevation_m) || !std::isfinite(mapped)
        || mapped < 0.0 || mapped > static_cast<double>(V9_MAX_HEIGHT))
    {
        throw std::out_of_range{"V9 source elevation maps outside the fixed gameplay Z domain [0,1023]"};
    }
    return static_cast<uint16_t>(mapped);
}

uint16_t mapV9WaterLevelToBlock(double source_water_level_m)
{
    return mapV9ElevationToBlock(source_water_level_m);
}

uint16_t mapV21ElevationToBlock(double const source_elevation_m)
{
    double const mapped = static_cast<double>(V21_BLOCK_SEA_LEVEL)
        + 0.34 * source_elevation_m;
    double const height = std::floor(mapped);
    if (!std::isfinite(source_elevation_m) || !std::isfinite(mapped)
        || height < 1.0 || height > static_cast<double>(V21_MAX_HEIGHT))
    {
        throw std::out_of_range{"V21 source elevation is outside its 0.34 block metric and 2048-block domain"};
    }
    return static_cast<uint16_t>(height);
}

uint16_t mapV21WaterLevelToBlock(double const source_water_level_m)
{
    double const mapped = static_cast<double>(V21_BLOCK_SEA_LEVEL)
        + 0.34 * source_water_level_m;
    double const height = std::floor(mapped);
    if (!std::isfinite(source_water_level_m) || !std::isfinite(mapped)
        || height < 0.0 || height > static_cast<double>(V21_MAX_HEIGHT))
    {
        throw std::out_of_range{"V21 source water level is outside its 0.34 block metric and 2048-block domain"};
    }
    return static_cast<uint16_t>(height);
}

} // namespace generation
