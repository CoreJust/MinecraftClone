#include <server/WorldPreparation.hpp>
#include <shared/corelang/Backend.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/net/Message.hpp>
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

server::WorldPreparationSnapshot waitForTerminal(
    server::WorldPreparation& preparation,
    std::chrono::seconds const timeout = 60s
)
{
    auto const deadline = server::WorldPreparation::Clock::now() + timeout;
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

std::shared_ptr<generation::GenerationSnapshot const> makeV9Snapshot(
    generation::GenerationStage const stage,
    float const marker
)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t SAMPLE_COUNT = RESOLUTION * RESOLUTION;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = stage;
    snapshot->resolution = RESOLUTION;
    snapshot->v9.emplace();
    auto& fields = *snapshot->v9;
    fields.source_elevation_m.assign(SAMPLE_COUNT, marker);
    fields.plate_ids.assign(SAMPLE_COUNT, 1U);
    fields.plate_directions.push_back({ .plate_id = 1U, .x = 1.0F, .y = -1.0F });
    fields.plate_directions_observed_after_native_step = 1U;
    if (stage >= generation::GenerationStage::V9Climate) {
        fields.temperature_celsius.assign(SAMPLE_COUNT, 0.0F);
    }
    if (stage >= generation::GenerationStage::V9Water) {
        fields.precipitation_mm_year.assign(SAMPLE_COUNT, 0.0F);
        fields.water_level_m.assign(SAMPLE_COUNT, 0.0F);
        fields.ocean_mask.assign(SAMPLE_COUNT, 0U);
        fields.lake_mask.assign(SAMPLE_COUNT, 0U);
        fields.river_mask.assign(SAMPLE_COUNT, 0U);
        fields.ice_surface_kind.assign(SAMPLE_COUNT, 0U);
    }
    EXPECT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    return snapshot;
}

TEST(WorldPreparationTest, CoalescesSnapshotFloodAndEncodesBoundedSourceFramesInOrder)
{
    static constexpr uint32_t CLIMATE_UPDATE_COUNT = 1'317U;
    static constexpr uint64_t MAX_ENCODED_BYTES = 6'000'000U;
    static constexpr std::array<generation::GenerationStage, 4U> SOURCE_STAGES{{
        generation::GenerationStage::V9Platec,
        generation::GenerationStage::V9Terrain,
        generation::GenerationStage::V9Climate,
        generation::GenerationStage::V9Water,
    }};
    server::WorldPreparationProgressJournal journal;
    std::array<std::shared_ptr<generation::GenerationSnapshot const>, SOURCE_STAGES.size()> snapshots;
    uint64_t source_sequence = 0U;
    auto const record = [&](generation::GenerationProgress progress) {
        journal.record({
            .sequence = ++source_sequence,
            .progress = std::move(progress),
            .observed_at = server::WorldPreparation::Clock::time_point{} + source_sequence * 1ms,
        });
    };

    for (uint32_t stage_index = 0U; stage_index < SOURCE_STAGES.size(); ++stage_index) {
        generation::GenerationStage const stage = SOURCE_STAGES[stage_index];
        record({ .stage = stage, .completed = 0U, .total = 1U });
        if (stage == generation::GenerationStage::V9Climate) {
            for (uint32_t completed = 1U; completed <= CLIMATE_UPDATE_COUNT; ++completed) {
                auto intermediate = std::make_shared<generation::GenerationSnapshot>();
                intermediate->stage = stage;
                intermediate->resolution = 256U;
                record({
                    .stage = stage,
                    .completed = completed,
                    .total = CLIMATE_UPDATE_COUNT,
                    .snapshot = std::move(intermediate),
                });
            }
        }
        snapshots[stage_index] = makeV9Snapshot(stage, static_cast<float>(stage_index + 1U));
        record({ .stage = stage, .completed = 1U, .total = 1U, .snapshot = snapshots[stage_index] });
    }
    record({ .stage = generation::GenerationStage::Complete, .completed = 1U, .total = 1U });

    auto const& milestones = journal.milestones();
    ASSERT_EQ(milestones.size(), 2U * SOURCE_STAGES.size() + 1U);
    EXPECT_LE(milestones.size(), server::WorldPreparationProgressJournal::MAX_MILESTONE_COUNT);
    std::vector<generation::GenerationStage> actual_source_stages;
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V9;
    std::string const configuration_identity = generation::configurationIdentity(configuration);
    shared::TerrainSessionIdentity const session{
        .join_epoch = 1U,
        .server_nonce = 2U,
        .terrain = {
            .terrain_version = generation::terrainVersion(generation::TerrainModelTag::V9),
            .revision = 1U,
            .seed = configuration.seed,
            .configuration_identity = configuration_identity,
        },
        .configuration = configuration,
    };
    uint64_t encoded_bytes = 0U;
    uint64_t message_sequence = 0U;
    uint32_t source_snapshot_count = 0U;
    std::shared_ptr<generation::GenerationSnapshot const> latest_snapshot;
    for (server::WorldPreparationProgressMilestone const& milestone : milestones) {
        if (milestone.kind == server::WorldPreparationProgressMilestoneKind::StageBoundary
            && milestone.update.progress.stage == generation::GenerationStage::Complete) {
            continue;
        }
        std::shared_ptr<generation::GenerationSnapshot const> snapshot =
            milestone.kind == server::WorldPreparationProgressMilestoneKind::SourceSnapshot
                ? milestone.update.progress.snapshot
                : nullptr;
        if (snapshot) {
            ++source_snapshot_count;
            latest_snapshot = snapshot;
            actual_source_stages.push_back(snapshot->stage);
        }
        shared::ServerWorldPreparationMessage message{
            .session = session,
            .sequence = ++message_sequence,
            .stage = milestone.update.progress.stage,
            .work_completed = milestone.update.progress.completed,
            .work_total = milestone.update.progress.total,
            .latest_snapshot = std::move(snapshot),
        };
        auto const encoded = shared::encodeMessage(shared::Message{ message });
        ASSERT_FALSE(encoded.empty());
        EXPECT_LE(encoded.size(), shared::MAX_MESSAGE_SIZE_BYTES);
        encoded_bytes += encoded.size();
    }
    ASSERT_EQ(source_snapshot_count, SOURCE_STAGES.size());
    EXPECT_EQ(actual_source_stages, (std::vector<generation::GenerationStage>{
        SOURCE_STAGES[0U], SOURCE_STAGES[1U], SOURCE_STAGES[2U], SOURCE_STAGES[3U]
    }));
    EXPECT_EQ(latest_snapshot, snapshots.back());
    shared::ServerWorldPreparationMessage const final_message{
        .session = session,
        .sequence = ++message_sequence,
        .stage = generation::GenerationStage::Complete,
        .work_completed = 1U,
        .work_total = 1U,
        .latest_snapshot = latest_snapshot,
        .final_atlas_complete = true,
    };
    auto const final_encoded = shared::encodeMessage(shared::Message{ final_message });
    ASSERT_FALSE(final_encoded.empty());
    encoded_bytes += final_encoded.size();
    EXPECT_EQ(final_message.latest_snapshot, snapshots.back());
    EXPECT_TRUE(final_message.final_atlas_complete);
    EXPECT_LE(encoded_bytes, MAX_ENCODED_BYTES);
}

TEST(WorldPreparationTest, ThrottlesRepeatedProgressButAdmitsBoundariesAndCompletion)
{
    static constexpr uint32_t CLIMATE_UPDATE_COUNT = 1'261U;
    server::WorldPreparationProgressThrottle throttle;
    auto const start = server::WorldPreparation::Clock::time_point{};
    uint32_t admitted = 0U;
    for (uint32_t completed = 0U; completed <= CLIMATE_UPDATE_COUNT; ++completed) {
        generation::GenerationProgress const progress{
            .stage = generation::GenerationStage::V9Climate,
            .completed = completed,
            .total = CLIMATE_UPDATE_COUNT,
        };
        admitted += throttle.admit(progress, start + completed * 1ms);
    }
    EXPECT_EQ(admitted, 7U);
    EXPECT_TRUE(throttle.admit({
        .stage = generation::GenerationStage::V9Water,
        .completed = 0U,
        .total = 10U,
    }, start + 1'262ms));
    EXPECT_TRUE(throttle.admit({
        .stage = generation::GenerationStage::V9Water,
        .completed = 10U,
        .total = 10U,
    }, start + 1'263ms));
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

TEST(WorldPreparationTest, PreservesNativeV9ModelAcrossLegacyCoreLangPlan)
{
    static constexpr uint64_t WORLD_REVISION = 21U;
    static constexpr std::chrono::seconds WORLD_PREPARATION_TIMEOUT{180};
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V9;
    server::WorldPreparation preparation{ configuration, WORLD_REVISION };
    server::WorldPreparationSnapshot const snapshot = waitForTerminal(
        preparation, WORLD_PREPARATION_TIMEOUT
    );

    ASSERT_EQ(snapshot.state, server::WorldPreparationState::Ready) << snapshot.error;
    ASSERT_NE(snapshot.ready_world, nullptr);
    ASSERT_TRUE(snapshot.terrain_plan.has_value());
    EXPECT_EQ(snapshot.terrain_plan->configuration, configuration);
    EXPECT_EQ(snapshot.ready_world->identity().revision, WORLD_REVISION);
    EXPECT_EQ(snapshot.ready_world->identity().configuration_identity,
        generation::configurationIdentity(configuration));
    ASSERT_NE(snapshot.latest_progress, std::nullopt);
    EXPECT_EQ(snapshot.latest_progress->progress.stage, generation::GenerationStage::Complete);
    ASSERT_NE(snapshot.latest_map_snapshot, nullptr);
    ASSERT_TRUE(snapshot.latest_map_snapshot->v9.has_value());
    EXPECT_EQ(snapshot.latest_map_snapshot->stage, generation::GenerationStage::V9Water);
    EXPECT_EQ(snapshot.latest_map_snapshot->v9->source_elevation_m.size(), 256U * 256U);
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
