#include <client/WorldLoading.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

[[nodiscard]] client::WorldLoadingIdentity identity(uint64_t const join_epoch = 1U)
{
    return client::WorldLoadingIdentity{
        .join_epoch = join_epoch,
        .configuration = generation::GenerationConfiguration{
            .seed = 42U,
            .resolution = 256U,
        },
    };
}

[[nodiscard]] shared::TerrainIdentity terrainIdentity(
    generation::GenerationConfiguration const& configuration
)
{
    return shared::TerrainIdentity{
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = 7U,
        .seed = configuration.seed,
        .configuration_identity = generation::configurationIdentity(configuration),
    };
}

[[nodiscard]] std::shared_ptr<generation::GlobalAtlas const> atlasFor(
    generation::GenerationConfiguration const& configuration
)
{
    uint32_t const sample_count = configuration.resolution * configuration.resolution;
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration = configuration;
    atlas->elevation.assign(sample_count, 1.0F);
    return atlas;
}

void materializeKeys(
    client::WorldLoading& loading,
    client::WorldLoadingIdentity const& world_identity,
    std::vector<shared::ChunkCoordinate> const& keys,
    Clock::time_point const published_at
)
{
    for (shared::ChunkCoordinate const key : keys) {
        shared::Chunk const chunk{ key };
        static_cast<void>(loading.publishMaterializedChunk(world_identity, chunk, published_at));
    }
}

TEST(WorldLoadingTest, ClosedSphereUsesThreeDimensionalChunkCorners)
{
    static constexpr uint32_t RADIUS_BLOCKS = 1U;
    static constexpr shared::PlayerPosition CENTER{ .x = 15, .y = 15, .z = 15 };
    static constexpr shared::ChunkCoordinate CORNER{ .x = 1, .y = 1, .z = 1 };

    std::vector<shared::ChunkCoordinate> const keys = client::WorldLoading::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );

    EXPECT_EQ(keys.size(), 4U);
    EXPECT_EQ(std::find(keys.begin(), keys.end(), CORNER), keys.end());

    std::vector<shared::ChunkCoordinate> const expanded = client::WorldLoading::requiredChunkKeys(
        CENTER,
        2U
    );
    EXPECT_NE(std::find(expanded.begin(), expanded.end(), CORNER), expanded.end());
}

TEST(WorldLoadingTest, SphereUsesNearestWrappedHorizontalChunkImages)
{
    static constexpr uint32_t RADIUS_BLOCKS = client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS;
    static constexpr shared::PlayerPosition CENTER{ .x = 0, .y = 0, .z = 8 };
    static constexpr shared::ChunkCoordinate WRAPPED_CORNER{ .x = 4'095, .y = 4'095, .z = 0 };

    std::vector<shared::ChunkCoordinate> const keys = client::WorldLoading::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );

    EXPECT_NE(std::find(keys.begin(), keys.end(), WRAPPED_CORNER), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), (shared::ChunkCoordinate{ .x = 0, .y = 0, .z = 0 })), keys.end());
}

TEST(WorldLoadingTest, ActualMapSnapshotsReplaceOlderSnapshotsAndRejectStaleWorlds)
{
    static constexpr Clock::time_point T0{};
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    client::WorldLoadingIdentity const current_identity = identity();
    client::WorldLoadingIdentity const stale_identity = identity(2U);
    ASSERT_TRUE(loading.begin(current_identity, T0));

    auto first_snapshot = std::make_shared<generation::GenerationSnapshot const>(
        generation::GenerationSnapshot{
            .stage = generation::GenerationStage::Tectonics,
            .resolution = 256U,
            .elevation = std::vector<float>(65'536U, 1.0F),
            .temperature = {},
            .precipitation = {},
            .water_level = {},
            .ocean_mask = {},
            .lake_mask = {},
            .river_mask = {},
        }
    );
    generation::GenerationProgress first_progress{
        .stage = generation::GenerationStage::Tectonics,
        .completed = 2U,
        .total = 12U,
        .snapshot = first_snapshot,
    };
    ASSERT_TRUE(loading.publishGenerationProgress(current_identity, first_progress, T0));

    auto second_snapshot = std::make_shared<generation::GenerationSnapshot const>(
        generation::GenerationSnapshot{
            .stage = generation::GenerationStage::CoastalElaboration,
            .resolution = 256U,
            .elevation = std::vector<float>(65'536U, 2.0F),
            .temperature = {},
            .precipitation = {},
            .water_level = std::vector<float>(65'536U, 3.0F),
            .ocean_mask = std::vector<uint8_t>(65'536U, 1U),
            .lake_mask = {},
            .river_mask = {},
        }
    );
    generation::GenerationProgress second_progress{
        .stage = generation::GenerationStage::CoastalElaboration,
        .completed = 1U,
        .total = 4U,
        .snapshot = second_snapshot,
    };
    ASSERT_TRUE(loading.publishGenerationProgress(current_identity, second_progress, T0 + 1s));

    generation::GenerationProgress stale_progress{
        .stage = generation::GenerationStage::FinalClimate,
        .completed = 6U,
        .total = 6U,
        .snapshot = {},
    };
    EXPECT_FALSE(loading.publishGenerationProgress(stale_identity, stale_progress, T0 + 2s));

    client::WorldLoadingStatus const status = loading.status(T0 + 2s);
    EXPECT_EQ(status.generation_stage, generation::GenerationStage::CoastalElaboration);
    EXPECT_EQ(status.generation_completed, 1U);
    EXPECT_EQ(status.generation_total, 4U);
    EXPECT_EQ(status.map_snapshot, second_snapshot);
}

TEST(WorldLoadingTest, ConfigurationBindingAndReconnectPreserveTheOriginalRequestClock)
{
    static constexpr Clock::time_point T0{};
    client::WorldLoading loading{ client::WorldLoadingPlatform::Windows };
    client::WorldLoadingIdentity request_identity = identity();
    ASSERT_TRUE(loading.begin(request_identity, T0));

    generation::GenerationConfiguration const actual_configuration{
        .seed = 99U,
        .resolution = 512U,
    };
    ASSERT_TRUE(loading.bindConfiguration(request_identity, actual_configuration, T0 + 3s));
    request_identity.configuration = actual_configuration;
    ASSERT_TRUE(loading.publishServerCreationElapsed(request_identity, 125'000U, T0 + 4s));

    client::WorldLoadingStatus const before_reconnect = loading.status(T0 + 4s);
    EXPECT_EQ(before_reconnect.request_accepted_at, T0);
    EXPECT_EQ(before_reconnect.ready_prompt_deadline, T0 + 4s);
    EXPECT_EQ(before_reconnect.server_creation_elapsed_microseconds, 125'000U);

    client::WorldLoadingIdentity const stale_identity = request_identity;
    request_identity.join_epoch = 2U;
    ASSERT_TRUE(loading.restart(request_identity, T0 + 5s));
    EXPECT_FALSE(loading.publishServerCreationElapsed(stale_identity, 126'000U, T0 + 6s));
    EXPECT_EQ(loading.status(T0 + 5s).request_accepted_at, T0);
    EXPECT_FALSE(loading.status(T0 + 5s).server_creation_elapsed_microseconds.has_value());
    EXPECT_EQ(loading.status(T0 + 5s).state, client::WorldLoadingState::Creating);
}

TEST(WorldLoadingTest, SpawnAndChunkEvidenceCannotOutrunTheFinalAtlasOrRequestIdentity)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::PlayerPosition SPAWN{ .x = 512, .y = 512, .z = 512 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    client::WorldLoadingIdentity const current_identity = identity();
    client::WorldLoadingIdentity const stale_identity = identity(2U);
    ASSERT_TRUE(loading.begin(current_identity, T0));

    EXPECT_FALSE(loading.publishRefinedSpawn(
        current_identity,
        SPAWN,
        client::SpawnValidation::Safe,
        T0
    ));
    EXPECT_FALSE(loading.publishMaterializedChunk(
        stale_identity,
        shared::Chunk{ { .x = 0, .y = 0, .z = 0 } },
        T0
    ));
    EXPECT_EQ(loading.status(T0).state, client::WorldLoadingState::Creating);
    EXPECT_FALSE(loading.status(T0).spawn_safe);

    ASSERT_TRUE(loading.publishAtlas(current_identity, atlasFor(current_identity.configuration), T0));
    EXPECT_EQ(loading.status(T0).state, client::WorldLoadingState::PreparingSpawn);
    ASSERT_TRUE(loading.publishRefinedSpawn(
        current_identity,
        SPAWN,
        client::SpawnValidation::Safe,
        T0
    ));
    EXPECT_TRUE(loading.status(T0).spawn_safe);
}

TEST(WorldLoadingTest, AtlasCompletionAttestationAllowsJoinReadinessWithoutClientAtlas)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::PlayerPosition SPAWN{ .x = 512, .y = 512, .z = 512 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    client::WorldLoadingIdentity const world_identity = identity();
    ASSERT_TRUE(loading.begin(world_identity, T0));

    EXPECT_TRUE(loading.publishAtlasCompletion(
        world_identity,
        terrainIdentity(world_identity.configuration),
        T0 + 1s
    ));
    client::WorldLoadingStatus const atlas_status = loading.status(T0 + 1s);
    EXPECT_TRUE(atlas_status.atlas_ready);
    EXPECT_FALSE(atlas_status.atlas);
    EXPECT_EQ(atlas_status.state, client::WorldLoadingState::PreparingSpawn);

    EXPECT_TRUE(loading.publishRefinedSpawn(
        world_identity,
        SPAWN,
        client::SpawnValidation::Safe,
        T0 + 1s
    ));
    EXPECT_TRUE(loading.status(T0 + 1s).spawn_safe);
}

TEST(WorldLoadingTest, KeyIsConsumedUntilTheReadyPromptWasActuallyPresented)
{
    static constexpr Clock::time_point T0{};
    static constexpr Clock::time_point READY_AT = T0 + std::chrono::seconds{ 1 };
    static constexpr Clock::time_point EVICTED_AT = READY_AT + 100ms;
    static constexpr Clock::time_point RESTORED_AT = READY_AT + 200ms;
    static constexpr shared::PlayerPosition SPAWN{ .x = 512, .y = 512, .z = 512 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    client::WorldLoadingIdentity const world_identity = identity();
    ASSERT_TRUE(loading.begin(world_identity, T0));
    ASSERT_TRUE(loading.publishAtlas(world_identity, atlasFor(world_identity.configuration), READY_AT));
    ASSERT_TRUE(loading.publishRefinedSpawn(
        world_identity,
        SPAWN,
        client::SpawnValidation::Safe,
        READY_AT
    ));

    std::vector<shared::ChunkCoordinate> const keys = client::WorldLoading::requiredChunkKeys(
        SPAWN,
        client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
    );
    ASSERT_GT(keys.size(), 1U);
    materializeKeys(loading, world_identity, keys, READY_AT);

    EXPECT_EQ(loading.status(READY_AT).state, client::WorldLoadingState::ReadyForStart);
    EXPECT_EQ(loading.acceptStartInput(world_identity, READY_AT), client::LoadingInputResult::Consumed);
    ASSERT_TRUE(loading.evictChunk(world_identity, keys.front(), EVICTED_AT));
    EXPECT_FALSE(loading.status(EVICTED_AT).initial_sphere_ready);
    EXPECT_FALSE(loading.status(EVICTED_AT).ready_prompt_frame_rendered_at.has_value());
    EXPECT_EQ(loading.acceptStartInput(world_identity, EVICTED_AT), client::LoadingInputResult::Consumed);
    ASSERT_TRUE(loading.publishMaterializedChunk(
        world_identity,
        shared::Chunk{ keys.front() },
        RESTORED_AT
    ));
    EXPECT_EQ(loading.status(RESTORED_AT).state, client::WorldLoadingState::ReadyForStart);
    ASSERT_TRUE(loading.markReadyPromptFrameRendered(world_identity, RESTORED_AT));
    EXPECT_EQ(loading.acceptStartInput(world_identity, READY_AT + 2s), client::LoadingInputResult::Started);

    client::WorldLoadingStatus const starting_status = loading.status(READY_AT + 2s);
    EXPECT_EQ(starting_status.state, client::WorldLoadingState::Starting);
    EXPECT_EQ(starting_status.key_accepted_at, READY_AT + 2s);
    EXPECT_FALSE(starting_status.start_acknowledged_at.has_value());
    EXPECT_TRUE(loading.acknowledgeStart(
        world_identity,
        true,
        {},
        READY_AT + 3s
    ));

    client::WorldLoadingStatus const status = loading.status(READY_AT + 3s);
    EXPECT_EQ(status.state, client::WorldLoadingState::Playing);
    EXPECT_EQ(status.request_accepted_at, T0);
    EXPECT_EQ(status.key_accepted_at, READY_AT + 2s);
    EXPECT_EQ(status.start_acknowledged_at, READY_AT + 3s);
    EXPECT_EQ(status.full_radius_deadline, T0 + 20s);
}

TEST(WorldLoadingTest, RejectedStartAcknowledgementFailsWithTheServerReason)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::PlayerPosition SPAWN{ .x = 512, .y = 512, .z = 512 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    client::WorldLoadingIdentity const world_identity = identity();
    ASSERT_TRUE(loading.begin(world_identity, T0));
    ASSERT_TRUE(loading.publishAtlas(world_identity, atlasFor(world_identity.configuration), T0));
    ASSERT_TRUE(loading.publishRefinedSpawn(
        world_identity,
        SPAWN,
        client::SpawnValidation::Safe,
        T0
    ));
    materializeKeys(
        loading,
        world_identity,
        client::WorldLoading::requiredChunkKeys(SPAWN, client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS),
        T0
    );
    ASSERT_TRUE(loading.markReadyPromptFrameRendered(world_identity, T0 + 1s));
    ASSERT_EQ(loading.acceptStartInput(world_identity, T0 + 1s), client::LoadingInputResult::Started);

    EXPECT_TRUE(loading.acknowledgeStart(
        world_identity,
        false,
        "The server rejected this world session.",
        T0 + 2s
    ));

    client::WorldLoadingStatus const status = loading.status(T0 + 2s);
    EXPECT_EQ(status.state, client::WorldLoadingState::Failed);
    EXPECT_EQ(status.error, "The server rejected this world session.");
    EXPECT_FALSE(status.start_acknowledged_at.has_value());
}

TEST(WorldLoadingTest, DeadlinesUseOriginalRequestTimeAndNeverAdvanceGenerationByTime)
{
    static constexpr Clock::time_point T0{};
    static constexpr auto WINDOWS_LATE = T0 + std::chrono::seconds{ 15 } + 1ms;
    client::WorldLoading loading{ client::WorldLoadingPlatform::Windows };
    ASSERT_TRUE(loading.begin(identity(), T0));

    client::WorldLoadingStatus const status = loading.status(WINDOWS_LATE);

    EXPECT_EQ(status.state, client::WorldLoadingState::Creating);
    EXPECT_FALSE(status.atlas_ready);
    EXPECT_FALSE(status.initial_sphere_ready);
    EXPECT_TRUE(status.ready_prompt_deadline_missed);
    EXPECT_TRUE(status.full_radius_deadline_missed);
    EXPECT_EQ(status.ready_prompt_deadline, T0 + 4s);
    EXPECT_EQ(status.full_radius_deadline, T0 + 15s);
}

TEST(WorldLoadingTest, DenseNearSphereDoesNotQualifyFullAuthoritativeTerrain)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::PlayerPosition CENTER{ .x = 512, .y = 512, .z = 512 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    auto const world_identity = identity();
    ASSERT_TRUE(loading.begin(world_identity, T0));
    ASSERT_TRUE(loading.publishAtlas(world_identity, atlasFor(world_identity.configuration), T0));
    ASSERT_TRUE(loading.publishRefinedSpawn(world_identity, CENTER, client::SpawnValidation::Safe, T0));
    materializeKeys(loading, world_identity, client::WorldLoading::requiredChunkKeys(
        CENTER, client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS), T0);

    auto const status = loading.status(T0);
    EXPECT_TRUE(status.initial_sphere_ready);
    EXPECT_EQ(status.state, client::WorldLoadingState::ReadyForStart);
    EXPECT_FALSE(status.playable_sphere_ready);
    EXPECT_EQ(status.resident_authoritative_columns, 0U);
    EXPECT_GT(status.required_authoritative_columns, 6'400U);
    EXPECT_EQ(status.actual_chunk_radius_blocks, 640U);
    EXPECT_EQ(client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS, 48U);
    EXPECT_THROW(static_cast<void>(client::WorldLoading::requiredChunkKeys(CENTER, 640U)), std::invalid_argument);
}

TEST(WorldLoadingTest, MissingEdgeProfilePreventsReadinessAndEvictionRemovesIt)
{
    static constexpr Clock::time_point T0{};
    static constexpr Clock::time_point PUBLISHED_AT = T0 + 14s;
    static constexpr shared::PlayerPosition CENTER{ .x = 1'024, .y = 1'024, .z = 512 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Windows };
    auto const world_identity = identity();
    ASSERT_TRUE(loading.begin(world_identity, T0));
    ASSERT_TRUE(loading.publishAtlas(world_identity, atlasFor(world_identity.configuration), T0));
    ASSERT_TRUE(loading.publishRefinedSpawn(world_identity, CENTER, client::SpawnValidation::Safe, T0));
    auto const columns = shared::requiredTerrainColumns(CENTER, 640U);
    ASSERT_FALSE(columns.empty());
    auto const edge = columns.back();
    for (auto const column : shared::requiredTerrainColumns(CENTER, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS)) {
        if (column != edge) {
            ASSERT_TRUE(loading.publishAuthoritativeColumn(world_identity, column, PUBLISHED_AT));
        }
    }
    EXPECT_FALSE(loading.status(PUBLISHED_AT).playable_sphere_ready);
    EXPECT_EQ(loading.status(PUBLISHED_AT).resident_authoritative_columns + 1U, columns.size());
    ASSERT_TRUE(loading.publishAuthoritativeColumn(world_identity, edge, PUBLISHED_AT));
    auto const ready = loading.status(T0 + 15s + 1ms);
    EXPECT_TRUE(ready.playable_sphere_ready);
    EXPECT_EQ(ready.playable_sphere_first_ready_at, PUBLISHED_AT);
    EXPECT_EQ(ready.required_authoritative_columns, columns.size());
    EXPECT_EQ(ready.playable_sphere_required_chunks, columns.size() * 64U);
    EXPECT_FALSE(ready.full_radius_deadline_missed);
    EXPECT_EQ(ready.full_radius_deadline, T0 + 15s);
    ASSERT_TRUE(loading.evictAuthoritativeColumn(world_identity, edge, T0 + 16s));
    EXPECT_FALSE(loading.status(T0 + 16s).playable_sphere_ready);
}

TEST(WorldLoadingTest, AuthoritativeTerrainFollowsMovementAndWrapsAtEveryAltitude)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::PlayerPosition CENTER{ .x = 0, .y = 0, .z = 1'400 };
    static constexpr shared::PlayerPosition MOVED{ .x = 16, .y = 0, .z = 60'000 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    auto const world_identity = identity();
    ASSERT_TRUE(loading.begin(world_identity, T0));
    ASSERT_TRUE(loading.publishAtlas(world_identity, atlasFor(world_identity.configuration), T0));
    ASSERT_TRUE(loading.publishRefinedSpawn(world_identity, CENTER, client::SpawnValidation::Safe, T0));
    EXPECT_TRUE(loading.status(T0).initial_sphere_ready);
    EXPECT_FALSE(loading.status(T0).playable_sphere_ready);
    auto const columns = shared::requiredTerrainColumns(CENTER, 640U);
    EXPECT_NE(std::ranges::find(columns, shared::ChunkCoordinate{ 4'095, 4'095, 0 }), columns.end());
    for (auto const column : shared::requiredTerrainColumns(CENTER, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS)) {
        ASSERT_TRUE(loading.publishAuthoritativeColumn(world_identity, column, T0));
    }
    EXPECT_TRUE(loading.status(T0).playable_sphere_ready);
    ASSERT_TRUE(loading.evictAuthoritativeColumn(world_identity, {41, 0, 0}, T0 + 1ms));
    ASSERT_TRUE(loading.updatePlayerPosition(world_identity, MOVED, T0 + 1ms));
    EXPECT_FALSE(loading.status(T0 + 1ms).playable_sphere_ready);
    for (auto const column : shared::requiredTerrainColumns(MOVED, 640U)) {
        ASSERT_TRUE(loading.publishAuthoritativeColumn(world_identity, column, T0 + 2ms));
    }
    EXPECT_TRUE(loading.status(T0 + 2ms).playable_sphere_ready);
}

TEST(WorldLoadingTest, AuthoritativeProfilesRejectStaleSessionsTimesAndNonzeroZ)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::ChunkCoordinate COLUMN{ 0, 0, 0 };
    static constexpr shared::ChunkCoordinate INVALID_COLUMN{ 0, 0, 1 };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    auto const old_identity = identity();
    auto const new_identity = identity(2U);
    ASSERT_TRUE(loading.begin(old_identity, T0));
    ASSERT_TRUE(loading.publishAuthoritativeColumn(old_identity, COLUMN, T0));
    EXPECT_FALSE(loading.publishAuthoritativeColumn(old_identity, INVALID_COLUMN, T0));
    ASSERT_TRUE(loading.restart(new_identity, T0 + 1s));
    EXPECT_FALSE(loading.publishAuthoritativeColumn(old_identity, COLUMN, T0 + 2s));
    EXPECT_FALSE(loading.evictAuthoritativeColumn(old_identity, COLUMN, T0 + 2s));
    EXPECT_FALSE(loading.publishAuthoritativeColumn(new_identity, COLUMN, T0));
    EXPECT_EQ(loading.status(T0 + 2s).resident_authoritative_columns, 0U);
    EXPECT_EQ(loading.status(T0 + 2s).request_accepted_at, T0);
    EXPECT_EQ(loading.status(T0 + 2s).full_radius_deadline, T0 + 20s);
    ASSERT_TRUE(loading.publishAtlas(new_identity, atlasFor(new_identity.configuration), T0 + 2s));
    ASSERT_TRUE(loading.publishRefinedSpawn(
        new_identity, { .x = 0, .y = 0, .z = 512 }, client::SpawnValidation::Safe, T0 + 2s
    ));
    EXPECT_EQ(loading.status(T0 + 2s).resident_authoritative_columns, 0U);
    ASSERT_TRUE(loading.publishAuthoritativeColumn(new_identity, COLUMN, T0 + 2s));
    EXPECT_FALSE(loading.evictAuthoritativeColumn(new_identity, COLUMN, T0 + 1s));
    EXPECT_FALSE(loading.evictAuthoritativeColumn(old_identity, COLUMN, T0 + 2s));
    EXPECT_EQ(loading.status(T0 + 2s).resident_authoritative_columns, 1U);
    EXPECT_FALSE(loading.evictAuthoritativeColumn(new_identity, INVALID_COLUMN, T0 + 2s));
}

TEST(WorldLoadingTest, FullReadinessWaitsForExactStartupReserveAndRearmsAfterProfileClear)
{
    static constexpr Clock::time_point T0{};
    static constexpr shared::PlayerPosition CENTER{.x = 1'024, .y = 1'024, .z = 512};
    client::WorldLoading loading{client::WorldLoadingPlatform::Mac};
    auto const session = identity();
    ASSERT_TRUE(loading.begin(session, T0));
    ASSERT_TRUE(loading.publishAtlas(session, atlasFor(session.configuration), T0));
    ASSERT_TRUE(loading.publishRefinedSpawn(session, CENTER, client::SpawnValidation::Safe, T0));
    materializeKeys(loading, session, shared::requiredChunkKeys(CENTER, 48U), T0 + 1s);
    EXPECT_TRUE(loading.status(T0 + 1s).initial_sphere_ready);
    EXPECT_EQ(loading.status(T0 + 1s).state, client::WorldLoadingState::ReadyForStart);
    auto const actual = shared::requiredTerrainColumns(CENTER);
    auto const reserve = shared::requiredTerrainColumns(CENTER, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS);
    for (auto const key : actual) {
        ASSERT_TRUE(loading.publishAuthoritativeColumn(session, key, T0 + 2s));
    }
    EXPECT_EQ(loading.status(T0 + 2s).resident_authoritative_columns, 6'561U);
    EXPECT_FALSE(loading.status(T0 + 2s).playable_sphere_ready);
    EXPECT_FALSE(loading.status(T0 + 2s).playable_sphere_first_ready_at);
    auto const missing = reserve.back();
    for (auto const key : reserve) {
        if (key != missing) {
            ASSERT_TRUE(loading.publishAuthoritativeColumn(session, key, T0 + 2s));
        }
    }
    shared::ChunkCoordinate const unrelated{0, 0, 0};
    ASSERT_TRUE(loading.publishAuthoritativeColumn(session, unrelated, T0 + 2s));
    EXPECT_FALSE(loading.status(T0 + 2s).playable_sphere_ready);
    EXPECT_FALSE(loading.publishAuthoritativeColumn(identity(2U), missing, T0 + 2s));
    ASSERT_TRUE(loading.evictAuthoritativeColumn(session, unrelated, T0 + 2s));
    ASSERT_TRUE(loading.publishAuthoritativeColumn(session, missing, T0 + 2s + 417ms));
    EXPECT_TRUE(loading.status(T0 + 3s).playable_sphere_ready);
    EXPECT_EQ(loading.status(T0 + 3s).playable_sphere_first_ready_at, T0 + 2s + 417ms);
    EXPECT_EQ(loading.status(T0 + 3s).startup_prefetch_primed_at, T0 + 2s + 417ms);
    EXPECT_EQ(loading.status(T0 + 3s).resident_authoritative_columns, 6'561U);
    shared::PlayerPosition const moved{.x = 1'040, .y = 1'024, .z = 512};
    ASSERT_TRUE(loading.updatePlayerPosition(session, moved, T0 + 3s));
    EXPECT_TRUE(loading.status(T0 + 3s).playable_sphere_ready);
    ASSERT_TRUE(loading.evictAuthoritativeColumn(session, reserve.front(), T0 + 3s));
    EXPECT_TRUE(loading.status(T0 + 3s).playable_sphere_ready);
    for (auto const key : reserve) {
        ASSERT_TRUE(loading.evictAuthoritativeColumn(session, key, T0 + 4s));
    }
    for (auto const key : shared::requiredTerrainColumns(moved)) {
        ASSERT_TRUE(loading.publishAuthoritativeColumn(session, key, T0 + 5s));
    }
    EXPECT_FALSE(loading.status(T0 + 5s).playable_sphere_ready);
    EXPECT_FALSE(loading.status(T0 + 5s).startup_prefetch_primed_at);
    for (auto const key : shared::requiredTerrainColumns(moved, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS)) {
        ASSERT_TRUE(loading.publishAuthoritativeColumn(session, key, T0 + 6s));
    }
    EXPECT_TRUE(loading.status(T0 + 6s).playable_sphere_ready);
    auto const restarted_session = identity(2U);
    ASSERT_TRUE(loading.restart(restarted_session, T0 + 7s));
    EXPECT_FALSE(loading.status(T0 + 7s).startup_prefetch_primed_at);
    ASSERT_TRUE(loading.publishAtlas(restarted_session, atlasFor(restarted_session.configuration), T0 + 7s));
    ASSERT_TRUE(loading.publishRefinedSpawn(
        restarted_session, moved, client::SpawnValidation::Safe, T0 + 7s
    ));
    for (auto const key : shared::requiredTerrainColumns(moved)) {
        ASSERT_TRUE(loading.publishAuthoritativeColumn(restarted_session, key, T0 + 7s));
    }
    EXPECT_FALSE(loading.status(T0 + 7s).playable_sphere_ready);
}

} // namespace
