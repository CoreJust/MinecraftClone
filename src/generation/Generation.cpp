#include <generation/Generation.hpp>

#include "GenerationStages.hpp"
#include "Numerics.hpp"
#include "V10ProviderInternal.hpp"
#include "V21ProviderInternal.hpp"
#include "V9ProviderInternal.hpp"

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
    if (configuration.model_tag != TerrainModelTag::R6
        && configuration.model_tag != TerrainModelTag::V9
        && configuration.model_tag != TerrainModelTag::V21
        && configuration.model_tag != TerrainModelTag::V10)
    {
        throw std::invalid_argument("terrain model tag is unsupported");
    }
    if (configuration.use_v10_fine_terrain && configuration.model_tag != TerrainModelTag::V10)
    {
        throw std::invalid_argument("V10 fine terrain requires the V10 terrain model");
    }
    if (configuration.model_tag == TerrainModelTag::V10)
    {
        if (configuration.seed != 42U || configuration.resolution != 256U)
        {
            throw std::invalid_argument("V10 supports only seed 42 and resolution 256");
        }
        return;
    }
    if (configuration.resolution != 256U && configuration.resolution != 512U)
    {
        throw std::invalid_argument("terrain display resolution must be 256 or 512");
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

template <typename SnapshotFactory>
void publishStage(
    ProgressCallback const& progress,
    GenerationStage stage,
    SnapshotFactory&& make_snapshot,
    std::stop_token const& cancellation
)
{
    detail::checkCancellation(cancellation);
    if (progress)
    {
        auto snapshot = std::make_shared<GenerationSnapshot const>(make_snapshot());
        detail::checkCancellation(cancellation);
        detail::reportProgress(
            progress,
            stage,
            1U,
            1U,
            std::move(snapshot)
        );
    }
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

bool isValidGenerationSnapshot(GenerationSnapshot const& snapshot) noexcept
{
    if (static_cast<uint8_t>(snapshot.stage) > static_cast<uint8_t>(GenerationStage::V10Water)
        || (snapshot.resolution != 256U && snapshot.resolution != 512U))
    {
        return false;
    }

    bool const is_v10_stage = snapshot.stage >= GenerationStage::V10Platec
        && snapshot.stage <= GenerationStage::V10Climate;
    if (snapshot.v10 || is_v10_stage || snapshot.stage == GenerationStage::V10Water)
    {
        return detail::isValidV10Snapshot(snapshot);
    }

    uint32_t const sample_count = snapshot.resolution * snapshot.resolution;
    auto const valid_float_field = [sample_count](std::vector<float> const& values, bool required) {
        return required ? values.size() == sample_count
            && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); })
            : values.empty() || (values.size() == sample_count
                && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); }));
    };
    auto const valid_mask = [sample_count](std::vector<uint8_t> const& values, bool required) {
        return required ? values.size() == sample_count
            && std::ranges::all_of(values, [](uint8_t const value) { return value <= 1U; })
            : values.empty() || (values.size() == sample_count
                && std::ranges::all_of(values, [](uint8_t const value) { return value <= 1U; }));
    };

    bool const is_v21_stage = snapshot.stage >= GenerationStage::V21Platec
        && snapshot.stage <= GenerationStage::V21Water;
    if (snapshot.v21 || is_v21_stage)
    {
        if (!snapshot.v21 || !is_v21_stage || snapshot.v9 || snapshot.resolution != 256U
            || !snapshot.elevation.empty() || !snapshot.temperature.empty()
            || !snapshot.precipitation.empty() || !snapshot.water_level.empty()
            || !snapshot.ocean_mask.empty() || !snapshot.lake_mask.empty() || !snapshot.river_mask.empty())
        {
            return false;
        }

        constexpr size_t V21_SAMPLE_COUNT = 256U * 256U;
        constexpr size_t V21_PHASE_COUNT = 8U;
        constexpr size_t V21_PHASE_SAMPLE_COUNT = V21_PHASE_COUNT * V21_SAMPLE_COUNT;
        V21GenerationSnapshotFields const& fields = *snapshot.v21;
        bool const has_climate = snapshot.stage >= GenerationStage::V21Climate;
        bool const has_water = snapshot.stage == GenerationStage::V21Water;
        auto const valid_native_float = [](std::vector<float> const& values, bool required) {
            return required ? values.size() == V21_SAMPLE_COUNT
                && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); })
                : values.empty();
        };
        auto const valid_phase_float = [](std::vector<float> const& values, bool required) {
            return required ? values.size() == V21_PHASE_SAMPLE_COUNT
                && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); })
                : values.empty();
        };
        auto const valid_optional_native_float = [](std::vector<float> const& values) {
            return values.empty() || (values.size() == V21_SAMPLE_COUNT
                && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); }));
        };
        auto const valid_optional_phase_float = [](std::vector<float> const& values) {
            return values.empty() || (values.size() == V21_PHASE_SAMPLE_COUNT
                && std::ranges::all_of(values, [](float const value) { return std::isfinite(value); }));
        };
        auto const valid_nonnegative_phase_float = [&valid_phase_float](
            std::vector<float> const& values, bool required
        ) {
            return valid_phase_float(values, required)
                && (!required || std::ranges::all_of(values, [](float const value) { return value >= 0.0F; }));
        };
        auto const valid_native_mask = [](std::vector<uint8_t> const& values, bool required) {
            return required ? values.size() == V21_SAMPLE_COUNT
                && std::ranges::all_of(values, [](uint8_t const value) { return value <= 1U; })
                : values.empty();
        };
        auto const valid_native_byte_field = [](std::vector<uint8_t> const& values, bool required) {
            return required ? values.size() == V21_SAMPLE_COUNT : values.empty();
        };
        if (fields.source_elevation_m.size() != V21_SAMPLE_COUNT
            || fields.plate_ids.size() != V21_SAMPLE_COUNT
            || fields.plate_directions.empty() || fields.plate_directions.size() > 1024U
            || fields.plate_directions_observed_after_native_step > 2400U
            || !std::ranges::all_of(fields.source_elevation_m, [](float const value) { return std::isfinite(value); })
            || !valid_native_float(fields.temperature_celsius, has_climate)
            || !valid_native_float(fields.wind_u, has_climate)
            || !valid_native_float(fields.wind_v, has_climate)
            || !valid_optional_native_float(fields.precipitation_mm_year)
            || !valid_optional_native_float(fields.current_u)
            || !valid_optional_native_float(fields.current_v)
            || !valid_phase_float(fields.phase_temperature_celsius, has_climate)
            || !valid_optional_phase_float(fields.phase_precipitation_mm_year)
            || !valid_nonnegative_phase_float(fields.land_snow_water_equivalent_m, has_water)
            || !valid_nonnegative_phase_float(fields.glacier_thickness_m, has_water)
            || !valid_nonnegative_phase_float(fields.sea_ice_thickness_m, has_water)
            || !valid_native_float(fields.water_level_blocks, has_water)
            || !valid_native_mask(fields.ocean_mask, has_water)
            || !valid_native_mask(fields.lake_mask, has_water)
            || !valid_native_mask(fields.river_mask, has_water)
            || !valid_native_byte_field(fields.biome_id, has_water)
            || (has_water
                ? fields.ice_surface_kind.size() != V21_SAMPLE_COUNT
                : !fields.ice_surface_kind.empty()))
        {
            return false;
        }
        if (fields.current_u.empty() != fields.current_v.empty()
            || (has_water && (fields.precipitation_mm_year.empty() || fields.phase_precipitation_mm_year.empty()))
            || (!fields.precipitation_mm_year.empty()
                && !std::ranges::all_of(fields.precipitation_mm_year, [](float const value) { return value >= 0.0F; }))
            || (!fields.phase_precipitation_mm_year.empty()
                && !std::ranges::all_of(fields.phase_precipitation_mm_year,
                    [](float const value) { return value >= 0.0F; })))
        {
            return false;
        }
        if (has_water
            && (!std::ranges::all_of(fields.biome_id, [](uint8_t const value) { return value <= 12U; })
                || !std::ranges::all_of(fields.ice_surface_kind, [](uint8_t const value) {
                    return value <= static_cast<uint8_t>(V21GenerationIceSurfaceKind::SeaIce);
                })))
        {
            return false;
        }
        for (size_t index = 0U; index < fields.plate_directions.size(); ++index)
        {
            GenerationPlateDirection const& direction = fields.plate_directions[index];
            if (!std::isfinite(direction.x) || !std::isfinite(direction.y))
            {
                return false;
            }
            for (size_t previous_index = 0U; previous_index < index; ++previous_index)
            {
                if (fields.plate_directions[previous_index].plate_id == direction.plate_id)
                {
                    return false;
                }
            }
        }
        return std::ranges::all_of(fields.plate_ids, [&fields](uint32_t const plate_id) {
            return std::ranges::any_of(fields.plate_directions, [plate_id](GenerationPlateDirection const& direction) {
                return direction.plate_id == plate_id;
            });
        });
    }

    if (!snapshot.v9)
    {
        if (snapshot.stage >= GenerationStage::V9Platec)
        {
            return false;
        }
        bool const has_legacy_field = !snapshot.elevation.empty() || !snapshot.temperature.empty()
            || !snapshot.precipitation.empty() || !snapshot.water_level.empty()
            || !snapshot.ocean_mask.empty() || !snapshot.lake_mask.empty() || !snapshot.river_mask.empty();
        return has_legacy_field
            && valid_float_field(snapshot.elevation, true)
            && valid_float_field(snapshot.temperature, false)
            && valid_float_field(snapshot.precipitation, false)
            && valid_float_field(snapshot.water_level, false)
            && valid_mask(snapshot.ocean_mask, false)
            && valid_mask(snapshot.lake_mask, false)
            && valid_mask(snapshot.river_mask, false);
    }

    if (snapshot.resolution != 256U || !snapshot.elevation.empty() || !snapshot.temperature.empty()
        || !snapshot.precipitation.empty() || !snapshot.water_level.empty()
        || !snapshot.ocean_mask.empty() || !snapshot.lake_mask.empty() || !snapshot.river_mask.empty())
    {
        return false;
    }

    V9GenerationSnapshotFields const& fields = *snapshot.v9;
    uint32_t const native_sample_count = 256U * 256U;
    bool const has_climate = snapshot.stage >= GenerationStage::V9Climate;
    bool const has_water = snapshot.stage >= GenerationStage::V9Water;
    if (fields.source_elevation_m.size() != native_sample_count
        || fields.plate_ids.size() != native_sample_count
        || fields.plate_directions.empty() || fields.plate_directions.size() > 1024U
        || fields.plate_directions_observed_after_native_step > 2400U
        || !std::ranges::all_of(fields.source_elevation_m, [](float const value) { return std::isfinite(value); })
        || !valid_float_field(fields.wind_u, false)
        || !valid_float_field(fields.wind_v, false)
        || !valid_float_field(fields.current_u, false)
        || !valid_float_field(fields.current_v, false)
        || fields.wind_u.empty() != fields.wind_v.empty()
        || fields.current_u.empty() != fields.current_v.empty()
        || !valid_float_field(fields.temperature_celsius, has_climate)
        || !valid_float_field(fields.precipitation_mm_year, has_water)
        || !valid_float_field(fields.water_level_m, has_water)
        || !valid_mask(fields.ocean_mask, has_water)
        || !valid_mask(fields.lake_mask, has_water)
        || !valid_mask(fields.river_mask, has_water)
        || (has_water
            ? fields.ice_surface_kind.size() != native_sample_count
            : !fields.ice_surface_kind.empty()))
    {
        return false;
    }
    if (has_water && !std::ranges::all_of(fields.ice_surface_kind, [](uint8_t const value) {
            return value <= static_cast<uint8_t>(V9GenerationIceSurfaceKind::WaterIce);
        }))
    {
        return false;
    }

    for (size_t index = 0U; index < fields.plate_directions.size(); ++index)
    {
        GenerationPlateDirection const& direction = fields.plate_directions[index];
        if (!std::isfinite(direction.x) || !std::isfinite(direction.y))
        {
            return false;
        }
        for (size_t previous_index = 0U; previous_index < index; ++previous_index)
        {
            if (fields.plate_directions[previous_index].plate_id == direction.plate_id)
            {
                return false;
            }
        }
    }
    return std::ranges::all_of(fields.plate_ids, [&fields](uint32_t const plate_id) {
        return std::ranges::any_of(fields.plate_directions, [plate_id](GenerationPlateDirection const& direction) {
            return direction.plate_id == plate_id;
        });
    });
}

GenerationCancelled::GenerationCancelled()
    : std::runtime_error("Terrain-v2 generation was cancelled")
{
}

std::string configurationIdentity(GenerationConfiguration const& configuration)
{
    validateConfiguration(configuration);
    if (configuration.model_tag == TerrainModelTag::V10)
    {
        std::string identity = std::string{V10_MODEL_IDENTITY}
            + ";seed=42;resolution=256;platec=1.4.3-direct;foundation=v1;fine-climate=v1"
              ";fresh-climate=v7;scale=1;datum=512;depth=2048;native-v10-r1;water=v5-fine-bed-v1"
              ";river=c8f9b851f7891e555165648d473b39df1ecffc9b73ae65cf5b14ed9f5ba04703";
        if (configuration.use_v10_fine_terrain)
        {
            identity.append(V10_FINE_TERRAIN_IDENTITY_SUFFIX);
        }
        return identity;
    }
    if (configuration.model_tag == TerrainModelTag::V9)
    {
        std::string identity = "terrain-v9;model=coherent-practical-macro-watershed-weather-v9";
        identity += ";water=v74;source-closure-sha256=";
        identity += "f7b6f593c025fe6fb389da4521f030522806d5c064235a2a1c6eddaa7d856de5";
        identity += ";seed=" + std::to_string(configuration.seed);
        identity += ";display-resolution=" + std::to_string(configuration.resolution);
        identity += ";mapping=floor(384+source_m/10)";
        identity += ";domain=0..1023;";
        identity += "native-v9-v74-r1";
        return identity;
    }
    if (configuration.model_tag == TerrainModelTag::V21)
    {
        std::string identity{V21_MODEL_IDENTITY};
        identity += ";src=8a8e3a5bb29881b152884f65eb26b0d0e5988cae39591bb2c2a126625108a9bd";
        identity += ";closure=4feecac718b0f6b6160cfb8d300abaaef062783dd3e3efcec0e9d2b7e15ff216";
        identity += ";seed=" + std::to_string(configuration.seed);
        identity += ";resolution=" + std::to_string(configuration.resolution);
        identity += ";scale=0.34;datum=384;depth=2048;shore-contact=v1;native-v21-r3";
        return identity;
    }
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

    if (configuration.model_tag == TerrainModelTag::V10)
    {
        return detail::prepareV10GlobalAtlas(configuration, cancellation_context, std::move(progress_context));
    }
    if (configuration.model_tag == TerrainModelTag::V9)
    {
        return detail::prepareV9GlobalAtlas(
            configuration,
            cancellation_context,
            std::move(progress_context)
        );
    }
    if (configuration.model_tag == TerrainModelTag::V21)
    {
        return detail::prepareV21GlobalAtlas(
            configuration,
            cancellation_context,
            std::move(progress_context)
        );
    }

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
        [&] {
            return makeSnapshot(GenerationStage::Tectonics, configuration.resolution, elevation);
        },
        cancellation_context
    );

    detail::FloatField activity(elevation.size());
    for (size_t index = 0U; index < elevation.size(); ++index)
    {
        if ((index & 4095U) == 0U)
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
        [&] {
            return makeSnapshot(
                GenerationStage::CoastalElaboration,
                configuration.resolution,
                elevation
            );
        },
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
    publishStage(
        progress_context,
        GenerationStage::InitialDrainage,
        [&] {
            GenerationSnapshot snapshot = makeSnapshot(
                GenerationStage::InitialDrainage,
                configuration.resolution,
                elevation
            );
            addHydrology(snapshot, initial_hydrology);
            return snapshot;
        },
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
    publishStage(
        progress_context,
        GenerationStage::ProvisionalClimate,
        [&] {
            GenerationSnapshot snapshot = makeSnapshot(
                GenerationStage::ProvisionalClimate,
                configuration.resolution,
                elevation
            );
            addHydrology(snapshot, initial_hydrology);
            addClimate(snapshot, provisional_climate);
            return snapshot;
        },
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
    publishStage(
        progress_context,
        GenerationStage::TerrainErosion,
        [&] {
            GenerationSnapshot snapshot = makeSnapshot(
                GenerationStage::TerrainErosion,
                configuration.resolution,
                elevation
            );
            addHydrology(snapshot, erosion);
            addClimate(snapshot, provisional_climate);
            return snapshot;
        },
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
    publishStage(
        progress_context,
        GenerationStage::FinalClimate,
        [&] {
            GenerationSnapshot snapshot = makeSnapshot(
                GenerationStage::FinalClimate,
                configuration.resolution,
                elevation
            );
            addHydrology(snapshot, erosion);
            addClimate(snapshot, climate);
            return snapshot;
        },
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
        if ((index & 4095U) == 0U)
        {
            detail::checkCancellation(cancellation_context);
        }
        final_hydrology.fjord_candidate_mask[index] = static_cast<uint8_t>(
            erosion.fjord_candidate_mask[index] & final_hydrology.ocean_mask[index]
        );
    }
    publishStage(
        progress_context,
        GenerationStage::FinalDrainage,
        [&] {
            GenerationSnapshot snapshot = makeSnapshot(
                GenerationStage::FinalDrainage,
                configuration.resolution,
                final_hydrology.elevation
            );
            addHydrology(snapshot, final_hydrology);
            addClimate(snapshot, climate);
            return snapshot;
        },
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

bool isValidAtlas(GlobalAtlas const& atlas) noexcept
{
    if (atlas.configuration.model_tag == TerrainModelTag::V10)
    {
        return detail::isValidV10Atlas(atlas);
    }
    if (atlas.v10_provider)
    {
        return false;
    }
    if (atlas.configuration.model_tag == TerrainModelTag::V9)
    {
        return atlas.v9_provider != nullptr
            && atlas.block_sea_level == V9_BLOCK_SEA_LEVEL
            && atlas.max_height == V9_MAX_HEIGHT
            && atlas.elevation.empty()
            && atlas.temperature.empty()
            && atlas.precipitation.empty();
    }
    if (atlas.configuration.model_tag == TerrainModelTag::V21)
    {
        return atlas.v21_provider != nullptr
            && atlas.v9_provider == nullptr
            && atlas.block_sea_level == V21_BLOCK_SEA_LEVEL
            && atlas.max_height == V21_MAX_HEIGHT
            && atlas.elevation.empty()
            && atlas.temperature.empty()
            && atlas.precipitation.empty();
    }
    if (atlas.configuration.model_tag != TerrainModelTag::R6
        || atlas.v9_provider != nullptr
        || atlas.v21_provider != nullptr)
    {
        return false;
    }
    size_t const sample_count = static_cast<size_t>(atlas.configuration.resolution)
        * atlas.configuration.resolution;
    return atlas.elevation.size() == sample_count
        && atlas.temperature.size() == sample_count
        && atlas.precipitation.size() == sample_count;
}

} // namespace generation
