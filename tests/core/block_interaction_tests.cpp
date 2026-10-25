#include <shared/world/BlockInteraction.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t WORLD_REVISION = 1U;
constexpr uint64_t WORLD_SEED = 42U;

shared::SparseWorld makeMaterializedOnlyWorld()
{
    return shared::SparseWorld(
        {.seed = WORLD_SEED, .max_resident_chunks = 8U, .revision = WORLD_REVISION},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
}

void publishChunk(shared::SparseWorld& world, shared::Chunk chunk)
{
    ASSERT_TRUE(world.publishMaterializedChunk(std::move(chunk), WORLD_REVISION, WORLD_SEED));
}

shared::Chunk makeChunk(std::initializer_list<std::pair<shared::BlockCoordinate, shared::Block>> const blocks)
{
    shared::Chunk chunk;
    for (auto const& [coordinate, block] : blocks) {
        EXPECT_TRUE(chunk.setBlock(coordinate, block));
    }
    return chunk;
}

} // namespace

TEST(BlockInteractionTest, ModeDefaultsKeepActionGrantsSeparateAndApplyMaterialRules)
{
    shared::EffectiveInteractionRights const spectator = shared::effectiveInteractionRights(shared::GameMode::Spectator);
    shared::EffectiveInteractionRights const creative = shared::effectiveInteractionRights(shared::GameMode::Creative);
    shared::EffectiveInteractionRights const survival = shared::effectiveInteractionRights(shared::GameMode::Survival);

    EXPECT_TRUE(spectator.allows(shared::InteractionAction::Flight));
    EXPECT_TRUE(spectator.allows(shared::InteractionAction::CollisionBypass));
    EXPECT_FALSE(spectator.allows(shared::InteractionAction::Break));
    EXPECT_FALSE(spectator.allows(shared::InteractionAction::Place));

    EXPECT_TRUE(creative.allows(shared::InteractionAction::Flight));
    EXPECT_FALSE(creative.allows(shared::InteractionAction::CollisionBypass));
    EXPECT_TRUE(creative.allows(shared::InteractionAction::QuickBreak));
    EXPECT_TRUE(creative.canBreak(shared::Block::Bedrock));
    EXPECT_TRUE(creative.canPlace(shared::Block::Stone));
    EXPECT_TRUE(creative.canPlace(shared::Block::Sand));

    EXPECT_FALSE(survival.allows(shared::InteractionAction::Flight));
    EXPECT_FALSE(survival.allows(shared::InteractionAction::CollisionBypass));
    EXPECT_FALSE(survival.allows(shared::InteractionAction::QuickBreak));
    EXPECT_TRUE(survival.canBreak(shared::Block::Stone));
    EXPECT_TRUE(survival.canBreak(shared::Block::Sand));
    EXPECT_FALSE(survival.canBreak(shared::Block::Bedrock));
    EXPECT_FALSE(survival.canPlace(shared::Block::Air));
}

TEST(BlockInteractionTest, SoftActionOverridesApplyBeforeHardVetoes)
{
    shared::InteractionPolicy policy;
    policy.soft_actions.set(shared::InteractionAction::Flight, true);
    policy.soft_actions.set(shared::InteractionAction::Break, true);

    shared::EffectiveInteractionRights const soft_granted =
        shared::effectiveInteractionRights(shared::GameMode::Spectator, policy);
    EXPECT_TRUE(soft_granted.allows(shared::InteractionAction::Flight));
    EXPECT_TRUE(soft_granted.canBreak(shared::Block::Stone));

    policy.hard_vetoes.set(shared::InteractionAction::Flight);
    policy.hard_vetoes.set(shared::InteractionAction::Break);

    shared::EffectiveInteractionRights const rights =
        shared::effectiveInteractionRights(shared::GameMode::Spectator, policy);

    EXPECT_FALSE(rights.allows(shared::InteractionAction::Flight));
    EXPECT_FALSE(rights.canBreak(shared::Block::Stone));
}

TEST(BlockInteractionTest, BreakAndPlaceFiltersAreIndependentAndHandleEmptyLists)
{
    shared::InteractionPolicy policy;
    policy.break_materials.whitelist = std::vector<shared::Block>{};
    policy.place_materials.blacklist = {shared::Block::Stone};

    shared::EffectiveInteractionRights const rights =
        shared::effectiveInteractionRights(shared::GameMode::Creative, policy);

    EXPECT_FALSE(rights.canBreak(shared::Block::Stone));
    EXPECT_TRUE(rights.canPlace(shared::Block::Dirt));
    EXPECT_FALSE(rights.canPlace(shared::Block::Stone));

    shared::InteractionPolicy empty_blacklist_policy;
    empty_blacklist_policy.break_materials.whitelist = std::vector<shared::Block>{shared::Block::Stone};
    EXPECT_TRUE(shared::effectiveInteractionRights(shared::GameMode::Creative, empty_blacklist_policy)
                    .canBreak(shared::Block::Stone));
}

TEST(BlockInteractionTest, BreakDurationsCountOnlyFullIntervalsAfterTheStartTick)
{
    shared::EffectiveInteractionRights const survival = shared::effectiveInteractionRights(shared::GameMode::Survival);

    struct DurationCase final {
        shared::Block block;
        uint16_t intervals;
    };
    static constexpr DurationCase CASES[] = {
        {shared::Block::Snow, 10U},
        {shared::Block::Dirt, 15U},
        {shared::Block::Grass, 15U},
        {shared::Block::Sand, 15U},
        {shared::Block::Ice, 20U},
        {shared::Block::Stone, 30U},
    };

    for (DurationCase const test_case : CASES) {
        SCOPED_TRACE(static_cast<int>(test_case.block));
        auto const timing = shared::breakTiming(survival, test_case.block);
        ASSERT_TRUE(timing.has_value());
        EXPECT_FALSE(timing->quick);
        EXPECT_EQ(timing->required_full_intervals_after_start, test_case.intervals);
        EXPECT_FALSE(shared::isBreakComplete(*timing, 0U));
        EXPECT_FALSE(shared::isBreakComplete(*timing, static_cast<uint16_t>(test_case.intervals - 1U)));
        EXPECT_TRUE(shared::isBreakComplete(*timing, test_case.intervals));
    }

    EXPECT_FALSE(shared::breakTiming(survival, shared::Block::Bedrock).has_value());
    EXPECT_FALSE(shared::breakTiming(survival, shared::Block::Air).has_value());
}

TEST(BlockInteractionTest, CreativeQuickBreakIncludesBedrockOnItsFirstAuthorizedTick)
{
    shared::EffectiveInteractionRights const creative = shared::effectiveInteractionRights(shared::GameMode::Creative);
    auto const timing = shared::breakTiming(creative, shared::Block::Bedrock);

    ASSERT_TRUE(timing.has_value());
    EXPECT_TRUE(timing->quick);
    EXPECT_EQ(timing->required_full_intervals_after_start, 0U);
    EXPECT_TRUE(shared::isBreakComplete(*timing, 0U));
}

TEST(BlockInteractionTest, UnknownAndPreviewStopWithoutMaterializingChunks)
{
    shared::SparseWorld unknown_world = makeMaterializedOnlyWorld();
    shared::BlockTargetResult const unknown = shared::traceBlockTarget(
        unknown_world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        5.0
    );
    EXPECT_EQ(unknown.status, shared::BlockTargetStatus::Unknown);
    EXPECT_FALSE(unknown.target.has_value());
    EXPECT_EQ(unknown_world.residentChunkCount(), 0U);

    shared::SparseWorld preview_world = makeMaterializedOnlyWorld();
    publishChunk(preview_world, makeChunk({}));
    shared::HeightTile preview{.coordinate = {.x = 1, .y = 0}};
    ASSERT_TRUE(preview_world.publishPreview({.x = 1, .y = 0, .z = 0}, preview, WORLD_REVISION, WORLD_SEED));

    shared::BlockTargetResult const preview_result = shared::traceBlockTarget(
        preview_world,
        {.origin_x = 15.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        1.0
    );
    EXPECT_EQ(preview_result.status, shared::BlockTargetStatus::Preview);
    EXPECT_FALSE(preview_result.target.has_value());
    EXPECT_EQ(preview_world.residentChunkCount(), 1U);
}

TEST(BlockInteractionTest, FindsTheFirstSolidVoxelAndIncludesTheReachBoundary)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({
        {{.x = 5U, .y = 0U, .z = 0U}, shared::Block::Stone},
    }));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 0.0, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        5.0
    );

    ASSERT_EQ(result.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(result.target.has_value());
    EXPECT_EQ(result.target->cell, (shared::WorldCoordinate{5, 0, 0}));
    EXPECT_EQ(result.target->block, shared::Block::Stone);
    EXPECT_EQ(result.target->entered_face, shared::BlockFace::NegativeX);
    EXPECT_DOUBLE_EQ(result.target->distance, 5.0);
}

TEST(BlockInteractionTest, DefaultInteractionReachIncludesSixBlocksAndRejectsSeven)
{
    static constexpr double EXPECTED_DEFAULT_REACH = 6.0;
    EXPECT_DOUBLE_EQ(shared::DEFAULT_BLOCK_REACH, EXPECTED_DEFAULT_REACH);

    shared::VoxelRay const eye_ray{
        .origin_y = 1.0,
        .origin_z = 0.5,
        .direction_y = 1.0,
    };

    shared::SparseWorld six_blocks_away = makeMaterializedOnlyWorld();
    publishChunk(six_blocks_away, makeChunk({{{.x = 0U, .y = 7U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const six_block_target = shared::resolveInteractionTarget(
        six_blocks_away,
        eye_ray,
        shared::InteractionPerspective::FirstPerson
    );

    shared::SparseWorld seven_blocks_away = makeMaterializedOnlyWorld();
    publishChunk(seven_blocks_away, makeChunk({{{.x = 0U, .y = 8U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const seven_block_target = shared::resolveInteractionTarget(
        seven_blocks_away,
        eye_ray,
        shared::InteractionPerspective::FirstPerson
    );

    ASSERT_TRUE(six_block_target.target.has_value());
    EXPECT_EQ(six_block_target.target->cell, (shared::WorldCoordinate{0, 7, 0}));
    EXPECT_EQ(seven_block_target.status, shared::BlockTargetStatus::Miss);
    EXPECT_FALSE(seven_block_target.target.has_value());
}

TEST(BlockInteractionTest, StopsAtTheFirstSolidVoxel)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({
        {{.x = 3U, .y = 0U, .z = 0U}, shared::Block::Dirt},
        {{.x = 5U, .y = 0U, .z = 0U}, shared::Block::Stone},
    }));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 0.0, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        5.0
    );

    ASSERT_EQ(result.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(result.target.has_value());
    EXPECT_EQ(result.target->cell, (shared::WorldCoordinate{3, 0, 0}));
    EXPECT_EQ(result.target->block, shared::Block::Dirt);
}

TEST(BlockInteractionTest, StopsAtAnUnknownCellBeforeAVisibleSolid)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({}));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 15.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        5.0
    );

    EXPECT_EQ(result.status, shared::BlockTargetStatus::Unknown);
    EXPECT_FALSE(result.target.has_value());
    EXPECT_EQ(world.residentChunkCount(), 1U);
}

TEST(BlockInteractionTest, TraversesAPlayerEditedThreeDimensionalCavity)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({
        {{.x = 0U, .y = 0U, .z = 0U}, shared::Block::Stone},
        {{.x = 0U, .y = 0U, .z = 2U}, shared::Block::Stone},
    }));

    // The air voxel at z=1 represents an explicit edit, not a generated cave.
    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 1.5, .direction_z = 1.0},
        5.0
    );

    ASSERT_EQ(result.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(result.target.has_value());
    EXPECT_EQ(result.target->cell, (shared::WorldCoordinate{0, 0, 2}));
    EXPECT_EQ(result.target->entered_face, shared::BlockFace::NegativeZ);
    EXPECT_DOUBLE_EQ(result.target->distance, 0.5);
}

TEST(BlockInteractionTest, AdvancesEveryAxisAtAnExactCornerAndChoosesTheXFaceForPlacement)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({
        {{.x = 1U, .y = 0U, .z = 0U}, shared::Block::Dirt},
        {{.x = 0U, .y = 1U, .z = 0U}, shared::Block::Dirt},
        {{.x = 1U, .y = 1U, .z = 0U}, shared::Block::Dirt},
        {{.x = 0U, .y = 0U, .z = 1U}, shared::Block::Dirt},
        {{.x = 1U, .y = 0U, .z = 1U}, shared::Block::Dirt},
        {{.x = 0U, .y = 1U, .z = 1U}, shared::Block::Dirt},
        {{.x = 1U, .y = 1U, .z = 1U}, shared::Block::Stone},
    }));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0, .direction_y = 1.0, .direction_z = 1.0},
        5.0
    );

    ASSERT_EQ(result.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(result.target.has_value());
    EXPECT_EQ(result.target->cell, (shared::WorldCoordinate{1, 1, 1}));
    EXPECT_EQ(result.target->entered_face, shared::BlockFace::NegativeX);
}

TEST(BlockInteractionTest, ReturnsNoFaceWhenTheEyeStartsInsideASolidCell)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({{{.x = 0U, .y = 0U, .z = 0U}, shared::Block::Stone}}));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        5.0
    );

    ASSERT_EQ(result.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(result.target.has_value());
    EXPECT_EQ(result.target->cell, (shared::WorldCoordinate{0, 0, 0}));
    EXPECT_EQ(result.target->entered_face, shared::BlockFace::None);
    EXPECT_DOUBLE_EQ(result.target->distance, 0.0);
}

TEST(BlockInteractionTest, ChoosesTheCellEnteredWhenTheRayStartsOnAFace)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({
        {{.x = 1U, .y = 0U, .z = 0U}, shared::Block::Dirt},
        {{.x = 2U, .y = 0U, .z = 0U}, shared::Block::Stone},
    }));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {.origin_x = 2.0, .origin_y = 0.5, .origin_z = 0.5, .direction_x = -1.0},
        5.0
    );

    ASSERT_EQ(result.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(result.target.has_value());
    EXPECT_EQ(result.target->cell, (shared::WorldCoordinate{1, 0, 0}));
    EXPECT_EQ(result.target->block, shared::Block::Dirt);
    EXPECT_EQ(result.target->entered_face, shared::BlockFace::PositiveX);
    EXPECT_DOUBLE_EQ(result.target->distance, 0.0);
}

TEST(BlockInteractionTest, WrapsHorizontalTargetsAndRejectsVerticalWorldBounds)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    shared::Chunk wrapped_chunk{{.x = 0, .y = 0, .z = 0}};
    ASSERT_TRUE(wrapped_chunk.setBlock({.x = 0U, .y = 0U, .z = 0U}, shared::Block::Stone));
    publishChunk(world, shared::Chunk{{.x = 4095, .y = 0, .z = 0}});
    publishChunk(world, std::move(wrapped_chunk));

    shared::BlockTargetResult const wrapped = shared::traceBlockTarget(
        world,
        {.origin_x = 65'535.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        1.0
    );
    ASSERT_EQ(wrapped.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(wrapped.target.has_value());
    EXPECT_EQ(wrapped.target->cell, (shared::WorldCoordinate{0, 0, 0}));

    shared::SparseWorld vertical_world = makeMaterializedOnlyWorld();
    publishChunk(vertical_world, makeChunk({}));
    shared::BlockTargetResult const vertical = shared::traceBlockTarget(
        vertical_world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 0.5, .direction_z = -1.0},
        1.0
    );
    EXPECT_EQ(vertical.status, shared::BlockTargetStatus::OutOfBounds);
    EXPECT_FALSE(vertical.target.has_value());
}

TEST(BlockInteractionTest, RejectsInvalidRaysAndDoesNotReadBeyondTheInclusiveReach)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({{{.x = 6U, .y = 0U, .z = 0U}, shared::Block::Stone}}));

    shared::BlockTargetResult const too_far = shared::traceBlockTarget(
        world,
        {.origin_x = 0.0, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        5.0
    );
    EXPECT_EQ(too_far.status, shared::BlockTargetStatus::Miss);

    shared::BlockTargetResult const zero_direction = shared::traceBlockTarget(
        world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 0.5},
        5.0
    );
    EXPECT_EQ(zero_direction.status, shared::BlockTargetStatus::InvalidRay);

    shared::BlockTargetResult const negative_reach = shared::traceBlockTarget(
        world,
        {.origin_x = 0.5, .origin_y = 0.5, .origin_z = 0.5, .direction_x = 1.0},
        -1.0
    );
    EXPECT_EQ(negative_reach.status, shared::BlockTargetStatus::InvalidRay);
}

TEST(BlockInteractionTest, InvalidGameModesCannotAcquireRightsFromSoftOverrides)
{
    shared::InteractionPolicy policy;
    policy.soft_actions.set(shared::InteractionAction::Break, true);
    policy.soft_actions.set(shared::InteractionAction::Flight, true);

    shared::EffectiveInteractionRights const rights = shared::effectiveInteractionRights(
        static_cast<shared::GameMode>(255U),
        policy
    );

    EXPECT_FALSE(rights.allows(shared::InteractionAction::Break));
    EXPECT_FALSE(rights.allows(shared::InteractionAction::Flight));
    EXPECT_FALSE(rights.canBreak(shared::Block::Stone));
}

TEST(BlockInteractionTest, FiniteLargeRayDirectionsAreNormalizedWithoutOverflow)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({{{.x = 1U, .y = 1U, .z = 0U}, shared::Block::Stone}}));

    shared::BlockTargetResult const result = shared::traceBlockTarget(
        world,
        {
            .origin_x = 0.5,
            .origin_y = 0.5,
            .origin_z = 0.5,
            .direction_x = 1.7e308,
            .direction_y = 1.7e308,
        },
        5.0
    );

    EXPECT_EQ(result.status, shared::BlockTargetStatus::Hit);
}

TEST(BlockInteractionTest, ReconstructsTheClientCameraCenterRayForEveryPerspective)
{
    shared::VoxelRay const eye_ray{
        .origin_x = 12.0,
        .origin_y = 20.0,
        .origin_z = 1.625,
        .direction_y = 1.0,
    };

    auto const first_person = shared::reconstructInteractionCameraRay(
        eye_ray,
        shared::InteractionPerspective::FirstPerson
    );
    auto const rear = shared::reconstructInteractionCameraRay(
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );
    auto const front = shared::reconstructInteractionCameraRay(
        eye_ray,
        shared::InteractionPerspective::ThirdPersonFront
    );

    ASSERT_TRUE(first_person.has_value());
    ASSERT_TRUE(rear.has_value());
    ASSERT_TRUE(front.has_value());
    EXPECT_DOUBLE_EQ(first_person->origin_y, 20.0);
    EXPECT_DOUBLE_EQ(first_person->direction_y, 1.0);
    EXPECT_DOUBLE_EQ(rear->origin_y, 14.0);
    EXPECT_DOUBLE_EQ(rear->direction_y, 1.0);
    EXPECT_DOUBLE_EQ(front->origin_y, 26.0);
    EXPECT_DOUBLE_EQ(front->direction_y, -1.0);

    auto const clipped = shared::reconstructInteractionCameraRay(
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear,
        1.5
    );
    ASSERT_TRUE(clipped.has_value());
    EXPECT_DOUBLE_EQ(clipped->origin_y, 18.5);
    EXPECT_FALSE(shared::reconstructInteractionCameraRay(
        eye_ray,
        static_cast<shared::InteractionPerspective>(255U)
    ).has_value());
}

TEST(BlockInteractionTest, SharedLookDirectionMatchesClientYawPitchLimits)
{
    auto const direction = shared::interactionLookDirection(90.0, 30.0);
    ASSERT_TRUE(direction.has_value());
    EXPECT_NEAR((*direction)[0], 0.866'025'403'784, 1e-12);
    EXPECT_NEAR((*direction)[1], 0.0, 1e-12);
    EXPECT_NEAR((*direction)[2], 0.5, 1e-12);

    auto const clamped_pitch = shared::interactionLookDirection(0.0, 100.0);
    ASSERT_TRUE(clamped_pitch.has_value());
    EXPECT_NEAR((*clamped_pitch)[1], std::cos(89.0 * 0.017'453'292'519'943'295), 1e-12);
    EXPECT_NEAR((*clamped_pitch)[2], std::sin(89.0 * 0.017'453'292'519'943'295), 1e-12);
    EXPECT_FALSE(shared::interactionLookDirection(std::numeric_limits<double>::infinity(), 0.0).has_value());
}

TEST(BlockInteractionTest, ResolvesFirstRearAndFrontViewTargetsWithEyeReach)
{
    shared::VoxelRay const eye_ray{
        .origin_x = 8.5,
        .origin_y = 8.5,
        .origin_z = 0.5,
        .direction_y = 1.0,
    };

    shared::SparseWorld first_person_world = makeMaterializedOnlyWorld();
    publishChunk(first_person_world, makeChunk({{{.x = 8U, .y = 10U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const first_person = shared::resolveInteractionTarget(
        first_person_world,
        eye_ray,
        shared::InteractionPerspective::FirstPerson
    );

    shared::SparseWorld rear_world = makeMaterializedOnlyWorld();
    publishChunk(rear_world, makeChunk({{{.x = 8U, .y = 10U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const rear = shared::resolveInteractionTarget(
        rear_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );

    shared::SparseWorld front_world = makeMaterializedOnlyWorld();
    publishChunk(front_world, makeChunk({{{.x = 8U, .y = 6U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const front = shared::resolveInteractionTarget(
        front_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonFront
    );

    ASSERT_TRUE(first_person.target.has_value());
    ASSERT_TRUE(rear.target.has_value());
    ASSERT_TRUE(front.target.has_value());
    EXPECT_EQ(first_person.target->cell, (shared::WorldCoordinate{8, 10, 0}));
    EXPECT_EQ(rear.target->cell, (shared::WorldCoordinate{8, 10, 0}));
    EXPECT_EQ(front.target->cell, (shared::WorldCoordinate{8, 6, 0}));
}

TEST(BlockInteractionTest, ThirdPersonCannotGainReachOrSeeThroughTheCameraWall)
{
    shared::VoxelRay const eye_ray{
        .origin_x = 0.5,
        .origin_y = 8.0,
        .origin_z = 0.5,
        .direction_y = 1.0,
    };

    shared::SparseWorld in_reach_world = makeMaterializedOnlyWorld();
    publishChunk(in_reach_world, makeChunk({{{.x = 0U, .y = 13U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const in_reach = shared::resolveInteractionTarget(
        in_reach_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );
    ASSERT_TRUE(in_reach.target.has_value());
    EXPECT_EQ(in_reach.target->cell, (shared::WorldCoordinate{0, 13, 0}));

    shared::SparseWorld out_of_reach_world = makeMaterializedOnlyWorld();
    publishChunk(out_of_reach_world, makeChunk({{{.x = 0U, .y = 15U, .z = 0U}, shared::Block::Stone}}));
    shared::BlockTargetResult const out_of_reach = shared::resolveInteractionTarget(
        out_of_reach_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );
    EXPECT_EQ(out_of_reach.status, shared::BlockTargetStatus::Miss);
    EXPECT_FALSE(out_of_reach.target.has_value());

    shared::SparseWorld wall_world = makeMaterializedOnlyWorld();
    publishChunk(wall_world, makeChunk({
        {{.x = 0U, .y = 10U, .z = 0U}, shared::Block::Dirt},
        {{.x = 0U, .y = 13U, .z = 0U}, shared::Block::Stone},
    }));
    shared::BlockTargetResult const wall = shared::resolveInteractionTarget(
        wall_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );
    ASSERT_TRUE(wall.target.has_value());
    EXPECT_EQ(wall.target->cell, (shared::WorldCoordinate{0, 10, 0}));
}

TEST(BlockInteractionTest, ThirdPersonCameraBoomStopsAtUnknownAndPreviewData)
{
    shared::VoxelRay const eye_ray{
        .origin_x = 0.5,
        .origin_y = 8.5,
        .origin_z = 0.5,
        .direction_y = 1.0,
    };

    shared::SparseWorld unknown_world = makeMaterializedOnlyWorld();
    shared::BlockTargetResult const unknown = shared::resolveInteractionTarget(
        unknown_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );
    EXPECT_EQ(unknown.status, shared::BlockTargetStatus::Unknown);
    EXPECT_FALSE(unknown.target.has_value());
    EXPECT_EQ(unknown_world.residentChunkCount(), 0U);

    shared::SparseWorld preview_world = makeMaterializedOnlyWorld();
    shared::HeightTile preview{.coordinate = {.x = 0, .y = 0}};
    ASSERT_TRUE(preview_world.publishPreview({.x = 0, .y = 0, .z = 0}, preview, WORLD_REVISION, WORLD_SEED));
    shared::BlockTargetResult const preview_result = shared::resolveInteractionTarget(
        preview_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear
    );
    EXPECT_EQ(preview_result.status, shared::BlockTargetStatus::Preview);
    EXPECT_FALSE(preview_result.target.has_value());
}

TEST(BlockInteractionTest, CurvedCenterRayAppliesTheRendererInverseCurvature)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    publishChunk(world, makeChunk({{{.x = 4U, .y = 0U, .z = 1U}, shared::Block::Stone}}));

    shared::VoxelRay const center_ray{
        .origin_x = 0.5,
        .origin_y = 0.5,
        .origin_z = 0.99'995,
        .direction_x = 1.0,
    };
    shared::BlockTargetResult const flat = shared::traceBlockTarget(world, center_ray, 5.0);
    shared::BlockTargetResult const curved = shared::traceBlockTarget(world, center_ray, 5.0, true);

    EXPECT_EQ(flat.status, shared::BlockTargetStatus::Miss);
    ASSERT_EQ(curved.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(curved.target.has_value());
    EXPECT_EQ(curved.target->cell, (shared::WorldCoordinate{4, 0, 1}));
    EXPECT_EQ(curved.target->entered_face, shared::BlockFace::NegativeZ);
}

TEST(BlockInteractionTest, CurvedThirdPersonVisibilityUsesRendererSpaceEndpoint)
{
    shared::VoxelRay const eye_ray{
        .origin_x = 8.5,
        .origin_y = 8.5,
        .origin_z = 0.999'87,
        .direction_x = 1.0,
    };

    shared::SparseWorld unobstructed_world = makeMaterializedOnlyWorld();
    publishChunk(unobstructed_world, makeChunk({{{.x = 13U, .y = 8U, .z = 1U}, shared::Block::Stone}}));
    shared::BlockTargetResult const unobstructed = shared::resolveInteractionTarget(
        unobstructed_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear,
        true
    );
    ASSERT_EQ(unobstructed.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(unobstructed.target.has_value());
    EXPECT_EQ(unobstructed.target->cell, (shared::WorldCoordinate{13, 8, 1}));

    shared::SparseWorld wall_world = makeMaterializedOnlyWorld();
    publishChunk(wall_world, makeChunk({
        {{.x = 10U, .y = 8U, .z = 0U}, shared::Block::Dirt},
        {{.x = 13U, .y = 8U, .z = 1U}, shared::Block::Stone},
    }));
    shared::BlockTargetResult const obstructed = shared::resolveInteractionTarget(
        wall_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear,
        true
    );

    EXPECT_EQ(obstructed.status, shared::BlockTargetStatus::Miss);
    EXPECT_FALSE(obstructed.target.has_value());

    shared::BlockTargetResult const flat_wall = shared::resolveInteractionTarget(
        wall_world,
        eye_ray,
        shared::InteractionPerspective::ThirdPersonRear,
        false
    );
    ASSERT_EQ(flat_wall.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(flat_wall.target.has_value());
    EXPECT_EQ(flat_wall.target->cell, (shared::WorldCoordinate{10, 8, 0}));
}

TEST(BlockInteractionTest, FirstPersonPlacedBlockAtGlobalSpawnRemainsTargetable)
{
    shared::SparseWorld world = makeMaterializedOnlyWorld();
    shared::Chunk chunk{{.x = 2'048, .y = 2'048, .z = 28}};
    ASSERT_TRUE(chunk.setBlock({.x = 0U, .y = 2U, .z = 6U}, shared::Block::Stone));
    ASSERT_TRUE(chunk.setBlock({.x = 0U, .y = 2U, .z = 5U}, shared::Block::Grass));
    publishChunk(world, std::move(chunk));
    auto const direction = shared::interactionLookDirection(0.0, -35.0);
    ASSERT_TRUE(direction);
    shared::VoxelRay const ray{
        .origin_x = 32'768.3125, .origin_y = 32'768.3125, .origin_z = 455.625,
        .direction_x = (*direction)[0], .direction_y = (*direction)[1], .direction_z = (*direction)[2],
    };
    auto const raw = shared::traceBlockTarget(world, ray, shared::DEFAULT_BLOCK_REACH, true);
    ASSERT_EQ(raw.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(raw.target);
    ASSERT_EQ(raw.target->cell, (shared::WorldCoordinate{32'768, 32'770, 454}));
    auto const resolved = shared::resolveInteractionTarget(
        world, ray, shared::InteractionPerspective::FirstPerson, true);
    ASSERT_EQ(resolved.status, shared::BlockTargetStatus::Hit);
    ASSERT_TRUE(resolved.target);
    EXPECT_EQ(resolved.target->cell, raw.target->cell);
    EXPECT_EQ(resolved.target->entered_face, raw.target->entered_face);
}
