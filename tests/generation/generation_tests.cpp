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
        "terrain-v2;model-revision=5;source-map-sha256="
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
    constexpr size_t FIELD_SIZE = CONFIGURATION.resolution * CONFIGURATION.resolution;

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
