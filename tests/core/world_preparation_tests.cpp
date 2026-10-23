#include <server/WorldPreparation.hpp>
#include <shared/corelang/Backend.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

generation::GenerationConfiguration quickConfiguration()
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 8'800'555U;
    configuration.resolution = 256U;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    return configuration;
}

server::WorldPreparationSnapshot waitForTerminal(server::WorldPreparation& preparation)
{
    auto const deadline = server::WorldPreparation::Clock::now() + 60s;
    server::WorldPreparationSnapshot snapshot = preparation.latest();
    while (snapshot.state == server::WorldPreparationState::Preparing
        && server::WorldPreparation::Clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        snapshot = preparation.latest();
    }
    return snapshot;
}

bool waitForProgress(server::WorldPreparation& preparation)
{
    auto const deadline = server::WorldPreparation::Clock::now() + 5s;
    while (server::WorldPreparation::Clock::now() < deadline) {
        server::WorldPreparationSnapshot const snapshot = preparation.latest();
        if (snapshot.state != server::WorldPreparationState::Preparing) {
            return snapshot.latest_progress.has_value();
        }
        if (snapshot.latest_progress) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

TEST(WorldPreparationTest, PublishesValidatedTerrainWorldAndRetainsRealProgressAndT0)
{
    generation::GenerationConfiguration const configuration = quickConfiguration();
    auto const request_accepted_at = server::WorldPreparation::Clock::now() - 100ms;
    server::WorldPreparation preparation{ configuration, 17U, request_accepted_at };
    auto const snapshot = waitForTerminal(preparation);

    ASSERT_EQ(snapshot.state, server::WorldPreparationState::Ready);
    ASSERT_NE(snapshot.ready_world, nullptr);
    ASSERT_TRUE(snapshot.terrain_plan.has_value());
    EXPECT_EQ(snapshot.terrain_plan->receipt.actual_backend, shared::corelang::Backend::Interpreter);
    EXPECT_EQ(snapshot.terrain_plan->configuration, configuration);
    ASSERT_NE(snapshot.latest_progress, std::nullopt);
    ASSERT_NE(snapshot.latest_map_snapshot, nullptr);
    if (!snapshot.latest_progress) {
        return;
    }
    EXPECT_EQ(preparation.configuration(), configuration);
    EXPECT_EQ(snapshot.request_accepted_at, request_accepted_at);
    EXPECT_GE(snapshot.state_changed_at, snapshot.request_accepted_at);
    EXPECT_GE(snapshot.latest_progress->observed_at, snapshot.request_accepted_at);
    EXPECT_EQ(snapshot.latest_progress->progress.stage, generation::GenerationStage::Complete);
    EXPECT_LE(snapshot.latest_progress->progress.completed, snapshot.latest_progress->progress.total);
    EXPECT_EQ(snapshot.latest_map_snapshot->stage, generation::GenerationStage::FinalDrainage);
    EXPECT_EQ(snapshot.latest_map_snapshot->resolution, configuration.resolution);
    EXPECT_EQ(snapshot.latest_map_snapshot->elevation.size(), configuration.resolution * configuration.resolution);
    ASSERT_NE(snapshot.latest_map_snapshot_observed_at, std::nullopt);
    if (!snapshot.latest_map_snapshot_observed_at) {
        return;
    }
    EXPECT_GE(*snapshot.latest_map_snapshot_observed_at, snapshot.request_accepted_at);

    shared::TerrainWorld const& world = *snapshot.ready_world;
    EXPECT_EQ(world.identity().revision, 17U);
    EXPECT_EQ(world.identity().seed, configuration.seed);
    EXPECT_EQ(world.identity().configuration_identity, generation::configurationIdentity(configuration));
    EXPECT_EQ(world.atlas()->configuration, configuration);
    EXPECT_EQ(preparation.readyWorld(), snapshot.ready_world);
}

TEST(WorldPreparationTest, StrictBackendsPreserveCanonicalWorldAndOriginalRequestTime)
{
    generation::GenerationConfiguration const configuration = quickConfiguration();
    std::shared_ptr<generation::GlobalAtlas const> reference_atlas;
    static constexpr std::array BACKENDS{
        shared::corelang::Backend::Interpreter,
        shared::corelang::Backend::Jit,
        shared::corelang::Backend::Aot,
    };
    for (shared::corelang::Backend const backend : BACKENDS) {
        auto const accepted_at = server::WorldPreparation::Clock::now() - 100ms;
        server::WorldPreparation preparation{ configuration, 17U, accepted_at, backend };
        auto const snapshot = waitForTerminal(preparation);
#if defined(__linux__) && !defined(__ANDROID__)
        if (backend != shared::corelang::Backend::Interpreter) {
            EXPECT_EQ(snapshot.state, server::WorldPreparationState::Failed);
            EXPECT_EQ(snapshot.request_accepted_at, accepted_at);
            EXPECT_FALSE(snapshot.terrain_plan.has_value());
            EXPECT_EQ(snapshot.ready_world, nullptr);
            EXPECT_TRUE(snapshot.error.starts_with(
                "Terrain-v2 world preparation failed: CoreLang Terrain-v2 plan failed: "));
            continue;
        }
#endif
        ASSERT_EQ(snapshot.state, server::WorldPreparationState::Ready) << snapshot.error;
        ASSERT_TRUE(snapshot.terrain_plan.has_value());
        EXPECT_EQ(snapshot.request_accepted_at, accepted_at);
        EXPECT_EQ(snapshot.terrain_plan->receipt.requested_backend, backend);
        EXPECT_EQ(snapshot.terrain_plan->receipt.actual_backend, backend);
        EXPECT_EQ(snapshot.terrain_plan->configuration, configuration);
        EXPECT_TRUE(snapshot.terrain_plan->world_generation_plan.pre_generate);
        EXPECT_EQ(snapshot.terrain_plan->world_generation_plan.refinement_extents,
            (std::vector<uint32_t>{ 4'096U, 1'024U, 256U, 64U }));
        ASSERT_NE(snapshot.ready_world, nullptr);
        EXPECT_EQ(snapshot.ready_world->identity().configuration_identity,
            generation::configurationIdentity(configuration));
        auto const atlas = snapshot.ready_world->atlas();
        if (reference_atlas == nullptr) {
            reference_atlas = atlas;
        } else {
            EXPECT_TRUE(atlas->elevation == reference_atlas->elevation);
            EXPECT_TRUE(atlas->temperature == reference_atlas->temperature);
            EXPECT_TRUE(atlas->precipitation == reference_atlas->precipitation);
            EXPECT_TRUE(atlas->water_level == reference_atlas->water_level);
            EXPECT_TRUE(atlas->ocean_mask == reference_atlas->ocean_mask);
            EXPECT_TRUE(atlas->lake_mask == reference_atlas->lake_mask);
            EXPECT_TRUE(atlas->river_mask == reference_atlas->river_mask);
        }
    }
}

TEST(WorldPreparationTest, CancellationRequestsStopAndJoinsWithoutPublishingAWorld)
{
    generation::GenerationConfiguration configuration = quickConfiguration();
    configuration.epochs = 12U;
    server::WorldPreparation preparation{ configuration, 18U };
    ASSERT_TRUE(waitForProgress(preparation));
    auto const started_at = server::WorldPreparation::Clock::now();

    preparation.cancel();

    auto const elapsed = server::WorldPreparation::Clock::now() - started_at;
    auto const snapshot = preparation.latest();
    EXPECT_EQ(snapshot.state, server::WorldPreparationState::Cancelled);
    ASSERT_NE(snapshot.latest_progress, std::nullopt);
    EXPECT_EQ(snapshot.ready_world, nullptr);
    EXPECT_EQ(preparation.readyWorld(), nullptr);
    EXPECT_LE(elapsed, 5s);
    EXPECT_GE(snapshot.state_changed_at, snapshot.request_accepted_at);
}

TEST(WorldPreparationTest, InvalidGenerationConfigurationFailsWithActionableError)
{
    generation::GenerationConfiguration configuration = quickConfiguration();
    configuration.resolution = 128U;
    server::WorldPreparation preparation{ configuration, 19U };
    auto const snapshot = waitForTerminal(preparation);

    EXPECT_EQ(snapshot.state, server::WorldPreparationState::Failed);
    EXPECT_EQ(snapshot.ready_world, nullptr);
    EXPECT_EQ(preparation.readyWorld(), nullptr);
    EXPECT_NE(snapshot.error.find("resolution"), std::string::npos);
    EXPECT_GE(snapshot.state_changed_at, snapshot.request_accepted_at);
}

TEST(WorldPreparationTest, ZeroWorldRevisionFailsBeforeGenerationStarts)
{
    server::WorldPreparation preparation{ quickConfiguration(), 0U };
    auto const snapshot = preparation.latest();

    EXPECT_EQ(snapshot.state, server::WorldPreparationState::Failed);
    EXPECT_EQ(snapshot.ready_world, nullptr);
    EXPECT_EQ(snapshot.latest_progress, std::nullopt);
    EXPECT_NE(snapshot.error.find("revision"), std::string::npos);
    EXPECT_GE(snapshot.state_changed_at, snapshot.request_accepted_at);
}

} // namespace
