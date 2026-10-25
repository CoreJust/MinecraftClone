#include <generation/V9Provider.hpp>

#include "V9Climate.hpp"
#include "V9PlatecSource.hpp"
#include "V9ProviderInternal.hpp"
#include "V9Terrain.hpp"
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
    return snapshot;
}

[[nodiscard]]
std::shared_ptr<GenerationSnapshot const> makeV9WaterSnapshot(
    V9PlatecSourceResult const& platec,
    V9TerrainResult const& terrain,
    V9WaterResult const& water,
    GenerationStage const stage
)
{
    auto snapshot = std::make_shared<GenerationSnapshot>();
    snapshot->stage = stage;
    snapshot->resolution = NATIVE_RESOLUTION;
    snapshot->v9 = makeV9SnapshotFields(platec, terrain.final_bed_vertices_m);
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

[[nodiscard]]
std::vector<V9RegisteredFieldView> makeRegisteredFields(V9ProviderState const& state)
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
            throw std::runtime_error{"v9 production field registry is missing source field " + std::string{name}};
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
        throw std::logic_error{"v9 production registry count differs from the frozen 95-field contract"};
    }
    return views;
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
        makeV9WaterSnapshot(*platec, terrain, *water, GenerationStage::V9Water)
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

} // namespace generation
