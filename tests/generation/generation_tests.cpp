#include "Numerics.hpp"

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace
{

TEST(GenerationConfigurationTests, IdentityBindsAllParametersAndSourceRevision)
{
    constexpr generation::GenerationConfiguration CONFIGURATION;
    std::string const IDENTITY = generation::configurationIdentity(CONFIGURATION);

    EXPECT_EQ(
        IDENTITY,
        "terrain-v2;model-revision=6;source-map-sha256="
        "5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704"
        ";seed=42;resolution=256;epochs=12;erosion-steps=3"
        ";initial-climate-iterations=24;climate-iterations=80"
        ";currents=1;wind-sign=1;coastal-elaboration=1"
        ";shelf-exp=exp64-to-f32-v1"
    );
    EXPECT_LE(IDENTITY.size(), generation::CONFIGURATION_IDENTITY_MAX_BYTES);

    generation::GenerationConfiguration changed = CONFIGURATION;
    changed.seed += 1U;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.resolution += 256U;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.epochs -= 1U;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.erosion_steps -= 1U;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.initial_climate_iterations -= 1U;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.climate_iterations -= 1U;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.currents = false;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.wind_sign = -1;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
    changed = CONFIGURATION;
    changed.coastal_elaboration = false;
    EXPECT_NE(generation::configurationIdentity(changed), IDENTITY);
}

TEST(GenerationConfigurationTests, V21IdentityNamesReviewedShoreContactRevision)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V21;

    std::string const identity = generation::configurationIdentity(configuration);

    EXPECT_NE(identity.find("shore-contact=v1"), std::string::npos);
    EXPECT_NE(identity.find("native-v21-r3"), std::string::npos);
}

TEST(GenerationConfigurationTests, NativeV10FineTerrainHasASeparateSourceBoundIdentity)
{
    generation::GenerationConfiguration baseline;
    baseline.model_tag = generation::TerrainModelTag::V10;
    generation::GenerationConfiguration candidate = baseline;
    candidate.use_v10_fine_terrain = true;

    std::string const baseline_identity = generation::configurationIdentity(baseline);
    std::string const candidate_identity = generation::configurationIdentity(candidate);

    EXPECT_EQ(candidate_identity,
        baseline_identity + std::string{generation::V10_FINE_TERRAIN_IDENTITY_SUFFIX});
    EXPECT_EQ(baseline_identity.size(), 308U);
    EXPECT_EQ(candidate_identity.size(), 378U);
    EXPECT_LE(candidate_identity.size(), generation::CONFIGURATION_IDENTITY_MAX_BYTES);
    EXPECT_NE(candidate_identity, baseline_identity);
    candidate.model_tag = generation::TerrainModelTag::R6;
    EXPECT_THROW((void)generation::configurationIdentity(candidate), std::invalid_argument);
}

TEST(GenerationSnapshotTests, ValidatesV21StagePayloadUnitsAndFieldDomains)
{
    static constexpr size_t SAMPLE_COUNT = 256U * 256U;
    static constexpr size_t PHASE_SAMPLE_COUNT = 8U * SAMPLE_COUNT;
    generation::GenerationSnapshot snapshot;
    snapshot.stage = generation::GenerationStage::V21Water;
    snapshot.resolution = 256U;
    snapshot.v21.emplace();
    auto& fields = *snapshot.v21;
    fields.source_elevation_m.resize(SAMPLE_COUNT);
    fields.temperature_celsius.resize(SAMPLE_COUNT);
    fields.precipitation_mm_year.resize(SAMPLE_COUNT);
    fields.wind_u.resize(SAMPLE_COUNT);
    fields.wind_v.resize(SAMPLE_COUNT);
    fields.current_u.resize(SAMPLE_COUNT);
    fields.current_v.resize(SAMPLE_COUNT);
    fields.phase_temperature_celsius.resize(PHASE_SAMPLE_COUNT);
    fields.phase_precipitation_mm_year.resize(PHASE_SAMPLE_COUNT);
    fields.land_snow_water_equivalent_m.resize(PHASE_SAMPLE_COUNT);
    fields.glacier_thickness_m.resize(PHASE_SAMPLE_COUNT);
    fields.sea_ice_thickness_m.resize(PHASE_SAMPLE_COUNT);
    fields.water_level_blocks.resize(SAMPLE_COUNT);
    fields.ocean_mask.resize(SAMPLE_COUNT);
    fields.lake_mask.resize(SAMPLE_COUNT);
    fields.river_mask.resize(SAMPLE_COUNT);
    fields.biome_id.resize(SAMPLE_COUNT, 4U);
    fields.ice_surface_kind.resize(SAMPLE_COUNT,
        static_cast<uint8_t>(generation::V21GenerationIceSurfaceKind::None));
    fields.plate_ids.resize(SAMPLE_COUNT, 7U);
    fields.plate_directions.push_back({.plate_id = 7U, .x = 1.0F, .y = 0.0F});
    fields.plate_directions_observed_after_native_step = 100U;

    EXPECT_TRUE(generation::isValidGenerationSnapshot(snapshot));

    auto invalid = snapshot;
    invalid.v21->biome_id[0U] = 13U;
    EXPECT_FALSE(generation::isValidGenerationSnapshot(invalid));

    invalid = snapshot;
    invalid.v21->ocean_mask[0U] = 2U;
    EXPECT_FALSE(generation::isValidGenerationSnapshot(invalid));

    invalid = snapshot;
    invalid.v21->phase_precipitation_mm_year.pop_back();
    EXPECT_FALSE(generation::isValidGenerationSnapshot(invalid));

    invalid = snapshot;
    invalid.v9.emplace();
    EXPECT_FALSE(generation::isValidGenerationSnapshot(invalid));
}

TEST(GenerationNumericsTests, NumpyTanhMatchesPinnedFloat64Vectors)
{
    struct Vector
    {
        double input;
        uint64_t expected_bits;
    };

    static constexpr std::array<Vector, 10U> VECTORS{
        Vector{-0x1.4p+0, 0xbfeb'2523'bb6b'2deeULL},
        Vector{-0x1.8p-3, 0xbfc7'b8ff'903b'f776ULL},
        Vector{-0x1p-2, 0xbfcf'597e'a69a'1c86ULL},
        Vector{-0x1.8p-2, 0xbfd6'ef53'de8c'8fb0ULL},
        Vector{0x0p+0, 0x0000'0000'0000'0000ULL},
        Vector{0x1.8p-3, 0x3fc7'b8ff'903b'f776ULL},
        Vector{0x1p-2, 0x3fcf'597e'a69a'1c86ULL},
        Vector{0x1.8p-2, 0x3fd6'ef53'de8c'8fb0ULL},
        Vector{0x1.3c0ca428c59ddp+0, 0x3feb'0138'e4a9'c229ULL},
        Vector{0x1.8p+4, 0x3ff0'0000'0000'0000ULL},
    };

    for (Vector const& vector : VECTORS)
    {
        EXPECT_EQ(
            std::bit_cast<uint64_t>(generation::detail::numpyTanh(vector.input)),
            vector.expected_bits
        ) << "input=" << vector.input;
    }
}

TEST(GenerationPreparationTests, RejectsCancellationBeforePublishingProgress)
{
    std::stop_source CANCELLATION;
    CANCELLATION.request_stop();
    bool progress_received = false;

    EXPECT_THROW(
        static_cast<void>(generation::prepareAtlas(
            generation::GenerationConfiguration{},
            CANCELLATION.get_token(),
            [&progress_received](generation::GenerationProgress const&) {
                progress_received = true;
            }
        )),
        generation::GenerationCancelled
    );
    EXPECT_FALSE(progress_received);
}

TEST(GenerationPreparationTests, RejectsUnsupportedResolutionBeforeStarting)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = 128U;

    EXPECT_THROW(
        static_cast<void>(generation::prepareAtlas(configuration)),
        std::invalid_argument
    );
}

TEST(GenerationPreparationTests, PublishesImmutableMapsAtUsableStageBoundaries)
{
    constexpr generation::GenerationConfiguration CONFIGURATION;
    constexpr std::array<generation::GenerationStage, 7U> SNAPSHOT_STAGES{
        generation::GenerationStage::Tectonics,
        generation::GenerationStage::CoastalElaboration,
        generation::GenerationStage::InitialDrainage,
        generation::GenerationStage::ProvisionalClimate,
        generation::GenerationStage::TerrainErosion,
        generation::GenerationStage::FinalClimate,
        generation::GenerationStage::FinalDrainage,
    };
    constexpr size_t FIELD_SIZE = static_cast<size_t>(CONFIGURATION.resolution) * CONFIGURATION.resolution;

    std::vector<std::shared_ptr<generation::GenerationSnapshot const>> snapshots;
    std::vector<std::vector<float>> elevation_at_publication;
    bool completed = false;
    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
        CONFIGURATION,
        {},
        [&snapshots, &elevation_at_publication, &completed](
            generation::GenerationProgress const& progress
        ) {
            if (progress.stage == generation::GenerationStage::Complete) {
                completed = true;
                EXPECT_EQ(progress.snapshot, nullptr);
            }
            if (progress.snapshot != nullptr) {
                snapshots.push_back(progress.snapshot);
                elevation_at_publication.push_back(progress.snapshot->elevation);
            }
        }
    );

    ASSERT_NE(atlas, nullptr);
    ASSERT_EQ(snapshots.size(), SNAPSHOT_STAGES.size());
    ASSERT_EQ(elevation_at_publication.size(), SNAPSHOT_STAGES.size());
    for (size_t index = 0U; index < SNAPSHOT_STAGES.size(); ++index) {
        generation::GenerationSnapshot const& snapshot = *snapshots[index];
        EXPECT_EQ(snapshot.stage, SNAPSHOT_STAGES[index]);
        EXPECT_EQ(snapshot.resolution, CONFIGURATION.resolution);
        EXPECT_EQ(snapshot.elevation.size(), FIELD_SIZE);
        EXPECT_EQ(snapshot.elevation, elevation_at_publication[index]);
    }

    auto const snapshotFor = [&snapshots](generation::GenerationStage stage) {
        auto const iterator = std::find_if(
            snapshots.begin(),
            snapshots.end(),
            [stage](auto const& candidate) { return candidate->stage == stage; }
        );
        return iterator == snapshots.end()
            ? std::shared_ptr<generation::GenerationSnapshot const>{}
            : *iterator;
    };

    for (generation::GenerationStage const stage : std::array{
             generation::GenerationStage::InitialDrainage,
             generation::GenerationStage::ProvisionalClimate,
             generation::GenerationStage::TerrainErosion,
             generation::GenerationStage::FinalClimate,
             generation::GenerationStage::FinalDrainage,
         }) {
        auto const snapshot = snapshotFor(stage);
        ASSERT_NE(snapshot, nullptr);
        EXPECT_EQ(snapshot->water_level.size(), FIELD_SIZE);
        EXPECT_EQ(snapshot->ocean_mask.size(), FIELD_SIZE);
        EXPECT_EQ(snapshot->lake_mask.size(), FIELD_SIZE);
        EXPECT_EQ(snapshot->river_mask.size(), FIELD_SIZE);
    }

    for (generation::GenerationStage const stage : std::array{
             generation::GenerationStage::ProvisionalClimate,
             generation::GenerationStage::TerrainErosion,
             generation::GenerationStage::FinalClimate,
             generation::GenerationStage::FinalDrainage,
         }) {
        auto const snapshot = snapshotFor(stage);
        ASSERT_NE(snapshot, nullptr);
        EXPECT_EQ(snapshot->temperature.size(), FIELD_SIZE);
        EXPECT_EQ(snapshot->precipitation.size(), FIELD_SIZE);
    }

    EXPECT_EQ(snapshots.back()->elevation, atlas->elevation);
    EXPECT_TRUE(completed);
}

TEST(GenerationConfigurationTests, IdentityRejectsConfigurationsOutsideTheBoundedModel)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = 128U;

    EXPECT_THROW(
        static_cast<void>(generation::configurationIdentity(configuration)),
        std::invalid_argument
    );
}

} // namespace
