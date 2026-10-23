#include <generation/Generation.hpp>

#include "GenerationStages.hpp"
#include "Numerics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

namespace generation
{
namespace
{

void validateConfiguration(GenerationConfiguration const& configuration)
{
    if (configuration.resolution != 256U && configuration.resolution != 512U)
    {
        throw std::invalid_argument("Terrain-v2 atlas resolution must be 256 or 512");
    }
    if (configuration.epochs == 0U || configuration.epochs > 12U)
    {
        throw std::invalid_argument("Terrain-v2 tectonic epochs must be in [1,12]");
    }
    if (configuration.erosion_steps > 3U)
    {
        throw std::invalid_argument("Terrain-v2 erosion steps must be in [0,3]");
    }
    if (configuration.initial_climate_iterations == 0U
        || configuration.initial_climate_iterations > 24U)
    {
        throw std::invalid_argument("Terrain-v2 initial climate iterations must be in [1,24]");
    }
    if (configuration.climate_iterations == 0U || configuration.climate_iterations > 80U)
    {
        throw std::invalid_argument("Terrain-v2 final climate iterations must be in [1,80]");
    }
    if (configuration.wind_sign != -1 && configuration.wind_sign != 1)
    {
        throw std::invalid_argument("Terrain-v2 wind sign must be -1 or +1");
    }
}

GenerationSnapshot makeSnapshot(
    GenerationStage stage,
    uint32_t resolution,
    std::span<float const> elevation
)
{
    GenerationSnapshot snapshot;
    snapshot.stage = stage;
    snapshot.resolution = resolution;
    snapshot.elevation.assign(elevation.begin(), elevation.end());
    return snapshot;
}

void addHydrology(GenerationSnapshot& snapshot, detail::HydrologyFields const& hydrology)
{
    snapshot.water_level = hydrology.water_level;
    snapshot.ocean_mask = hydrology.ocean_mask;
    snapshot.lake_mask = hydrology.lake_mask;
    snapshot.river_mask = hydrology.river_mask;
}

void addClimate(GenerationSnapshot& snapshot, detail::ClimateFields const& climate)
{
    snapshot.temperature = climate.temperature;
    snapshot.precipitation = climate.precipitation;
}

void publishStage(
    ProgressCallback const& progress,
    GenerationStage stage,
    GenerationSnapshot snapshot,
    std::stop_token const& cancellation
)
{
    detail::checkCancellation(cancellation);
    detail::reportProgress(
        progress,
        stage,
        1U,
        1U,
        std::make_shared<GenerationSnapshot const>(std::move(snapshot))
    );
    detail::checkCancellation(cancellation);
}

void publishStage(
    ProgressCallback const& progress,
    GenerationStage stage,
    std::stop_token const& cancellation
)
{
    detail::checkCancellation(cancellation);
    detail::reportProgress(progress, stage, 1U, 1U);
    detail::checkCancellation(cancellation);
}

} // namespace

GenerationCancelled::GenerationCancelled()
    : std::runtime_error("Terrain-v2 generation was cancelled")
{
}

std::string configurationIdentity(GenerationConfiguration const& configuration)
{
    validateConfiguration(configuration);
    std::string identity = "terrain-v" + std::to_string(TERRAIN_VERSION);
    identity += ";model-revision=" + std::to_string(MODEL_REVISION);
    identity += ";source-map-sha256=";
    identity += MODEL_SOURCE_MAP_SHA256;
    identity += ";seed=" + std::to_string(configuration.seed);
    identity += ";resolution=" + std::to_string(configuration.resolution);
    identity += ";epochs=" + std::to_string(configuration.epochs);
    identity += ";erosion-steps=" + std::to_string(configuration.erosion_steps);
    identity += ";initial-climate-iterations="
        + std::to_string(configuration.initial_climate_iterations);
    identity += ";climate-iterations=" + std::to_string(configuration.climate_iterations);
    identity += ";currents=" + std::to_string(configuration.currents ? 1 : 0);
    identity += ";wind-sign=" + std::to_string(static_cast<int>(configuration.wind_sign));
    identity += ";coastal-elaboration="
        + std::to_string(configuration.coastal_elaboration ? 1 : 0);
    identity += ';';
    identity += NUMERICAL_KERNEL_IDENTITY;
    return identity;
}

std::shared_ptr<GlobalAtlas const> prepareAtlas(
    GenerationConfiguration const& configuration,
    std::stop_token cancellation,
    ProgressCallback progress
)
{
    std::stop_token cancellation_context = std::move(cancellation);
    ProgressCallback progress_context = std::move(progress);
    detail::checkCancellation(cancellation_context);
    validateConfiguration(configuration);

    detail::TectonicsFields geology = detail::generateTectonics(
        configuration,
        cancellation_context,
        progress_context
    );
    detail::checkCancellation(cancellation_context);

    detail::FloatField elevation = std::move(geology.elevation);
    float const sea_shift = detail::medianFloat32(elevation);
    for (float& value : elevation)
    {
        value -= sea_shift;
    }
    detail::FloatField tectonic_elevation = elevation;
    publishStage(
        progress_context,
        GenerationStage::Tectonics,
        makeSnapshot(GenerationStage::Tectonics, configuration.resolution, elevation),
        cancellation_context
    );

    detail::FloatField activity(elevation.size());
    for (size_t index = 0U; index < elevation.size(); ++index)
    {
        if ((index & 4'095U) == 0U)
        {
            detail::checkCancellation(cancellation_context);
        }
        float const relative_motion = geology.uplift[index] - geology.subduction[index]
            + std::abs(geology.divergence[index]);
        activity[index] = std::clamp(relative_motion / 2.5F, 0.0F, 1.0F);
    }

    detail::CoastalFields coast = detail::elaborateCoasts(
        configuration,
        elevation,
        activity,
        cancellation_context,
        progress_context
    );
    elevation = std::move(coast.elevation);
    publishStage(
        progress_context,
        GenerationStage::CoastalElaboration,
        makeSnapshot(GenerationStage::CoastalElaboration, configuration.resolution, elevation),
        cancellation_context
    );

    detail::HydrologyFields initial_hydrology = detail::buildHydrology(
        configuration,
        elevation,
        {},
        {},
        detail::HydrologyParameters{
            .erosion_steps = 0U,
            .sea_level = 0.0F,
            .stage = GenerationStage::InitialDrainage,
        },
        cancellation_context,
        progress_context
    );
    GenerationSnapshot initial_snapshot = makeSnapshot(
        GenerationStage::InitialDrainage,
        configuration.resolution,
        elevation
    );
    addHydrology(initial_snapshot, initial_hydrology);
    publishStage(
        progress_context,
        GenerationStage::InitialDrainage,
        std::move(initial_snapshot),
        cancellation_context
    );

    detail::ClimateFields provisional_climate = detail::simulateClimate(
        configuration,
        elevation,
        detail::ClimateParameters{
            .iterations = configuration.initial_climate_iterations,
            .sea_level = 0.0F,
        },
        initial_hydrology.ocean_mask,
        initial_hydrology.lake_mask,
        GenerationStage::ProvisionalClimate,
        cancellation_context,
        progress_context
    );
    GenerationSnapshot provisional_snapshot = makeSnapshot(
        GenerationStage::ProvisionalClimate,
        configuration.resolution,
        elevation
    );
    addHydrology(provisional_snapshot, initial_hydrology);
    addClimate(provisional_snapshot, provisional_climate);
    publishStage(
        progress_context,
        GenerationStage::ProvisionalClimate,
        std::move(provisional_snapshot),
        cancellation_context
    );

    detail::HydrologyFields erosion = detail::buildHydrology(
        configuration,
        elevation,
        provisional_climate.precipitation,
        provisional_climate.temperature,
        detail::HydrologyParameters{
            .erosion_steps = configuration.erosion_steps,
            .sea_level = 0.0F,
            .stage = GenerationStage::TerrainErosion,
        },
        cancellation_context,
        progress_context
    );
    elevation = erosion.elevation;
    GenerationSnapshot erosion_snapshot = makeSnapshot(
        GenerationStage::TerrainErosion,
        configuration.resolution,
        elevation
    );
    addHydrology(erosion_snapshot, erosion);
    addClimate(erosion_snapshot, provisional_climate);
    publishStage(
        progress_context,
        GenerationStage::TerrainErosion,
        std::move(erosion_snapshot),
        cancellation_context
    );

    detail::ClimateFields climate = detail::simulateClimate(
        configuration,
        elevation,
        detail::ClimateParameters{
            .iterations = configuration.climate_iterations,
            .sea_level = 0.0F,
        },
        erosion.ocean_mask,
        erosion.lake_mask,
        GenerationStage::FinalClimate,
        cancellation_context,
        progress_context
    );
    GenerationSnapshot climate_snapshot = makeSnapshot(
        GenerationStage::FinalClimate,
        configuration.resolution,
        elevation
    );
    addHydrology(climate_snapshot, erosion);
    addClimate(climate_snapshot, climate);
    publishStage(
        progress_context,
        GenerationStage::FinalClimate,
        std::move(climate_snapshot),
        cancellation_context
    );

    detail::HydrologyFields final_hydrology = detail::buildHydrology(
        configuration,
        elevation,
        climate.precipitation,
        climate.temperature,
        detail::HydrologyParameters{
            .erosion_steps = 0U,
            .sea_level = 0.0F,
            .stage = GenerationStage::FinalDrainage,
        },
        cancellation_context,
        progress_context
    );
    for (size_t index = 0U; index < final_hydrology.fjord_candidate_mask.size(); ++index)
    {
        if ((index & 4'095U) == 0U)
        {
            detail::checkCancellation(cancellation_context);
        }
        final_hydrology.fjord_candidate_mask[index] = static_cast<uint8_t>(
            erosion.fjord_candidate_mask[index] & final_hydrology.ocean_mask[index]
        );
    }
    GenerationSnapshot final_snapshot = makeSnapshot(
        GenerationStage::FinalDrainage,
        configuration.resolution,
        final_hydrology.elevation
    );
    addHydrology(final_snapshot, final_hydrology);
    addClimate(final_snapshot, climate);
    publishStage(
        progress_context,
        GenerationStage::FinalDrainage,
        std::move(final_snapshot),
        cancellation_context
    );

    auto const [minimum, maximum] = std::minmax_element(
        final_hydrology.elevation.begin(),
        final_hydrology.elevation.end()
    );
    double const below_sea = std::max(-static_cast<double>(*minimum), 1.0e-6);
    double const above_sea = std::max(static_cast<double>(*maximum), 1.0e-6);

    GlobalAtlas atlas;
    atlas.configuration = configuration;
    atlas.sea_level = 0.0F;
    atlas.block_sea_level = BLOCK_SEA_LEVEL;
    atlas.max_height = MAX_HEIGHT;
    atlas.vertical_scale = std::min(383.0 / below_sea, 636.0 / above_sea);
    atlas.elevation = std::move(final_hydrology.elevation);
    atlas.tectonic_elevation = std::move(tectonic_elevation);
    atlas.filled = std::move(final_hydrology.filled);
    atlas.water_level = std::move(final_hydrology.water_level);
    atlas.flow_accumulation = std::move(final_hydrology.flow_accumulation);
    atlas.downstream = std::move(final_hydrology.downstream);
    atlas.lake_id = std::move(final_hydrology.lake_id);
    atlas.outlet_id = std::move(final_hydrology.outlet_id);
    atlas.lake_mask = std::move(final_hydrology.lake_mask);
    atlas.river_mask = std::move(final_hydrology.river_mask);
    atlas.ocean_mask = std::move(final_hydrology.ocean_mask);
    atlas.fjord_candidate_mask = std::move(final_hydrology.fjord_candidate_mask);
    atlas.harbour_candidate_mask = std::move(final_hydrology.harbour_candidate_mask);
    atlas.temperature = std::move(climate.temperature);
    atlas.precipitation = std::move(climate.precipitation);
    atlas.moisture = std::move(climate.moisture);
    atlas.wind_u = std::move(climate.wind_u);
    atlas.wind_v = std::move(climate.wind_v);
    atlas.current_u = std::move(climate.current_u);
    atlas.current_v = std::move(climate.current_v);
    atlas.climate_zone = std::move(climate.climate_zone);
    atlas.snow_mask = std::move(climate.snow_mask);
    atlas.uplift = std::move(geology.uplift);
    atlas.island_arc_uplift = std::move(geology.island_arc_uplift);
    atlas.hotspot = std::move(geology.hotspot);
    atlas.subduction = std::move(geology.subduction);
    atlas.divergence = std::move(geology.divergence);
    atlas.continental = std::move(geology.continental);
    atlas.plate_vx = std::move(geology.plate_vx);
    atlas.plate_vy = std::move(geology.plate_vy);
    atlas.plate_id = std::move(geology.plate_id);

    publishStage(progress_context, GenerationStage::Complete, cancellation_context);
    return std::make_shared<GlobalAtlas const>(std::move(atlas));
}

} // namespace generation
