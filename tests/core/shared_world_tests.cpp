#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>

TEST(WorldTest, ExplicitSpawnAndLookup) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 3, 4 });
    auto const player = world.player(1);
    ASSERT_TRUE(player.has_value());
    EXPECT_EQ(player->x, 3u);
    EXPECT_EQ(player->y, 4u);
    EXPECT_TRUE(world.playerExists('@'));
    EXPECT_TRUE(world.playerByCharacter('@').has_value());
}

TEST(WorldTest, ExplicitSpawnRejectsOriginsOutsideFootprintBounds) {
    shared::World valid;
    valid.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 30, 30 });
    EXPECT_TRUE(valid.player(1).has_value());
    shared::World outside_x;
    outside_x.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 31, 4 });
    EXPECT_FALSE(outside_x.player(1).has_value());
    shared::World outside_y;
    outside_y.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 4, 31 });
    EXPECT_FALSE(outside_y.player(1).has_value());
}

TEST(WorldTest, MovePlayerRejectsUnknownAndOccupiedPositions) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });
    EXPECT_FALSE(world.movePlayer(99, { 1, 0 }));
    EXPECT_FALSE(world.movePlayer(1, { 127, 0 }));
    EXPECT_EQ(world.player(1)->x, 5u);
    EXPECT_EQ(world.player(1)->x_subcell, 0u);
}

TEST(WorldTest, BorderCellsAreValidAndCollisionChecksDoNotUnderflow) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 0, 0 });
    EXPECT_TRUE(world.movePlayer(1, { 127, 0 }));
    EXPECT_EQ(world.player(1)->x, 0u);
    EXPECT_EQ(world.player(1)->x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_TRUE(world.movePlayer(1, { static_cast<uint8_t>(-127), 0 }));
    EXPECT_EQ(world.player(1)->x, 0u);
    EXPECT_EQ(world.player(1)->x_subcell, 0u);
    EXPECT_FALSE(world.movePlayer(1, { static_cast<uint8_t>(-127), 0 }));
}

TEST(WorldTest, LastFootprintOriginCellIsValid) {
    shared::World world;
    world.spawnPlayer(
        1,
        '@',
        std::pair<uint8_t, uint8_t>{
            shared::World::MAX_PLAYER_ORIGIN_CELL,
            shared::World::MAX_PLAYER_ORIGIN_CELL,
        }
    );
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(world.player(1)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
}

TEST(WorldTest, RandomSpawnsStayWithinTheWorld) {
    shared::World world;
    for (shared::PlayerId id = 0; id < 8; ++id) {
        world.spawnPlayer(id, static_cast<char>('@' + id));
        ASSERT_TRUE(world.player(id).has_value());
        EXPECT_LE(world.player(id)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
        EXPECT_LE(world.player(id)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
        auto const spawned = world.player(id);
        for (auto const& other : world.players()) {
            if (other.id != id) {
                EXPECT_TRUE(
                    std::abs(static_cast<int32_t>(spawned->x) - other.x) > 1
                    || std::abs(static_cast<int32_t>(spawned->y) - other.y) > 1
                );
            }
        }
    }
}

TEST(WorldTest, FootprintMovementStopsAtTheLastValidOrigin) {
    static constexpr uint8_t LAST_INTERIOR_CELL = 29;
    static constexpr uint8_t POSITIVE_DIRECTION = 127;
    static constexpr uint16_t CARDINAL_START_SUBCELL =
        shared::SUBCELLS_PER_CELL - shared::MOVEMENT_SUBCELLS_PER_TICK;
    static constexpr uint16_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;
    static constexpr uint16_t DIAGONAL_START_SUBCELL = shared::SUBCELLS_PER_CELL - DIAGONAL_STEP;

    shared::World horizontal;
    horizontal.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ LAST_INTERIOR_CELL, 4 });
    horizontal.setPlayerPosition(1, LAST_INTERIOR_CELL, 4, CARDINAL_START_SUBCELL, 0);
    ASSERT_TRUE(horizontal.movePlayer(1, { POSITIVE_DIRECTION, 0 }));
    ASSERT_TRUE(horizontal.player(1).has_value());
    EXPECT_EQ(horizontal.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(horizontal.player(1)->x_subcell, 0U);
    EXPECT_FALSE(horizontal.movePlayer(1, { POSITIVE_DIRECTION, 0 }));
    EXPECT_EQ(horizontal.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(horizontal.player(1)->x_subcell, 0U);

    shared::World vertical;
    vertical.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 4, LAST_INTERIOR_CELL });
    vertical.setPlayerPosition(1, 4, LAST_INTERIOR_CELL, 0, CARDINAL_START_SUBCELL);
    ASSERT_TRUE(vertical.movePlayer(1, { 0, POSITIVE_DIRECTION }));
    ASSERT_TRUE(vertical.player(1).has_value());
    EXPECT_EQ(vertical.player(1)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(vertical.player(1)->y_subcell, 0U);
    EXPECT_FALSE(vertical.movePlayer(1, { 0, POSITIVE_DIRECTION }));
    EXPECT_EQ(vertical.player(1)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(vertical.player(1)->y_subcell, 0U);

    shared::World diagonal;
    diagonal.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ LAST_INTERIOR_CELL, LAST_INTERIOR_CELL });
    diagonal.setPlayerPosition(
        1,
        LAST_INTERIOR_CELL,
        LAST_INTERIOR_CELL,
        DIAGONAL_START_SUBCELL,
        DIAGONAL_START_SUBCELL
    );
    ASSERT_TRUE(diagonal.movePlayer(1, { POSITIVE_DIRECTION, POSITIVE_DIRECTION }));
    ASSERT_TRUE(diagonal.player(1).has_value());
    EXPECT_EQ(diagonal.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(diagonal.player(1)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(diagonal.player(1)->x_subcell, 0U);
    EXPECT_EQ(diagonal.player(1)->y_subcell, 0U);
    EXPECT_FALSE(diagonal.movePlayer(1, { POSITIVE_DIRECTION, POSITIVE_DIRECTION }));
    EXPECT_EQ(diagonal.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(diagonal.player(1)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(diagonal.player(1)->x_subcell, 0U);
    EXPECT_EQ(diagonal.player(1)->y_subcell, 0U);
}

TEST(WorldTest, CornerPlayersBlockAdjacentAxesAndSlideDiagonalApproach) {
    static constexpr std::array<std::pair<uint8_t, uint8_t>, 4> CORNERS{
        std::pair<uint8_t, uint8_t>{ 0, 0 },
        std::pair<uint8_t, uint8_t>{ shared::World::MAX_PLAYER_ORIGIN_CELL, 0 },
        std::pair<uint8_t, uint8_t>{ 0, shared::World::MAX_PLAYER_ORIGIN_CELL },
        std::pair<uint8_t, uint8_t>{
            shared::World::MAX_PLAYER_ORIGIN_CELL,
            shared::World::MAX_PLAYER_ORIGIN_CELL,
        },
    };
    for (auto const& [x, y] : CORNERS) {
        shared::World world;
        world.spawnPlayer(1, '@', {{ x, y }});
        auto const inside_x = static_cast<uint8_t>(x == 0 ? 2 : x - 2);
        auto const inside_y = static_cast<uint8_t>(y == 0 ? 2 : y - 2);
        auto const toward_x = static_cast<uint8_t>(x == 0 ? 255 : 1);
        auto const toward_y = static_cast<uint8_t>(y == 0 ? 255 : 1);
        world.spawnPlayer(2, '#', {{ inside_x, y }});
        EXPECT_FALSE(world.movePlayer(2, {
            static_cast<uint8_t>(static_cast<int8_t>(toward_x) * 127), 0,
        }));
        world.despawnPlayer(2);
        world.spawnPlayer(2, '#', {{ x, inside_y }});
        EXPECT_FALSE(world.movePlayer(2, {
            0, static_cast<uint8_t>(static_cast<int8_t>(toward_y) * 127),
        }));
        world.despawnPlayer(2);
        world.spawnPlayer(2, '#', {{ inside_x, inside_y }});
        EXPECT_TRUE(world.movePlayer(2, {
            static_cast<uint8_t>(static_cast<int8_t>(toward_x) * 127),
            static_cast<uint8_t>(static_cast<int8_t>(toward_y) * 127),
        }));
        ASSERT_TRUE(world.player(2).has_value());
        EXPECT_EQ(world.player(2)->y, inside_y);
        EXPECT_EQ(world.player(2)->y_subcell, 0U);
    }
}

TEST(WorldTest, ZeroDirectionDoesNotMovePlayer) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    EXPECT_TRUE(world.movePlayer(1, { 0, 0 }));
    EXPECT_EQ(world.player(1)->x, 5u);
    EXPECT_EQ(world.player(1)->y, 5u);
}

TEST(WorldTest, ZeroDurationNonzeroDirectionIsASuccessfulNoOp) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    EXPECT_TRUE(world.movePlayer(1, { 127, 0 }, std::chrono::milliseconds::zero()));
    EXPECT_EQ(world.player(1)->x, 5U);
    EXPECT_EQ(world.player(1)->x_subcell, 0U);
}

TEST(WorldTest, SubstepsAccumulateAndDiagonalSpeedIsNormalized) {
    shared::World cardinal;
    shared::World diagonal;
    cardinal.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    diagonal.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });

    ASSERT_TRUE(cardinal.movePlayer(1, { 127, 0 }, std::chrono::milliseconds{ 50 }));
    ASSERT_TRUE(cardinal.movePlayer(1, { 127, 0 }, std::chrono::milliseconds{ 50 }));
    ASSERT_TRUE(diagonal.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(cardinal.player(1).has_value());
    ASSERT_TRUE(diagonal.player(1).has_value());
    EXPECT_EQ(cardinal.player(1)->x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_EQ(cardinal.player(1)->y_subcell, 0u);
    uint32_t const diagonal_distance_squared = static_cast<uint32_t>(diagonal.player(1)->x_subcell)
        * diagonal.player(1)->x_subcell + static_cast<uint32_t>(diagonal.player(1)->y_subcell)
        * diagonal.player(1)->y_subcell;
    EXPECT_LE(diagonal_distance_squared, static_cast<uint32_t>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        * shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(WorldTest, FullTickMovesAtTheConfiguredSpeedAndRejectsAnOccupiedEndpoint) {
    shared::World moving_world;
    moving_world.spawnPlayer(1U, '@', std::pair<uint8_t, uint8_t>{ 5U, 5U });

    ASSERT_TRUE(moving_world.movePlayer(1U, { 127U, 0U }));
    ASSERT_TRUE(moving_world.player(1U).has_value());
    EXPECT_EQ(moving_world.player(1U)->x, 5U);
    EXPECT_EQ(moving_world.player(1U)->x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);

    shared::World blocked_world;
    blocked_world.spawnPlayer(1U, '@', std::pair<uint8_t, uint8_t>{ 5U, 5U });
    blocked_world.spawnPlayer(2U, '#', std::pair<uint8_t, uint8_t>{ 7U, 5U });

    EXPECT_FALSE(blocked_world.movePlayer(1U, { 127U, 0U }));
    ASSERT_TRUE(blocked_world.player(1U).has_value());
    EXPECT_EQ(blocked_world.player(1U)->x, 5U);
    EXPECT_EQ(blocked_world.player(1U)->x_subcell, 0U);
}

TEST(WorldTest, SubcellCollisionRejectsOverlapBeforeEitherPlayerChangesCells) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });
    world.setPlayerPosition(1, 5, 5, 5'000, 0);
    world.setPlayerPosition(2, 7, 5, 5'000, 0);

    EXPECT_FALSE(world.movePlayer(1, { 127, 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, 5U);
    EXPECT_EQ(world.player(1)->x_subcell, 5'000U);
}

TEST(WorldTest, DiagonalCornerResolvesXBeforeY) {
    static constexpr uint16_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;

    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 7 });

    EXPECT_TRUE(world.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x_subcell, DIAGONAL_STEP);
    EXPECT_EQ(world.player(1)->y_subcell, 0U);
}

TEST(WorldTest, HorizontalAndVerticalGlancingCollisionsSlideWithoutSpeedGain) {
    static constexpr uint16_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;

    shared::World horizontal_blocker;
    horizontal_blocker.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    horizontal_blocker.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });

    EXPECT_TRUE(horizontal_blocker.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(horizontal_blocker.player(1).has_value());
    EXPECT_EQ(horizontal_blocker.player(1)->x_subcell, 0U);
    EXPECT_EQ(horizontal_blocker.player(1)->y_subcell, DIAGONAL_STEP);

    shared::World vertical_blocker;
    vertical_blocker.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    vertical_blocker.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 5, 7 });

    EXPECT_TRUE(vertical_blocker.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(vertical_blocker.player(1).has_value());
    EXPECT_EQ(vertical_blocker.player(1)->x_subcell, DIAGONAL_STEP);
    EXPECT_EQ(vertical_blocker.player(1)->y_subcell, 0U);
}

TEST(WorldTest, BlockedStraightMoveCanMoveAway) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });

    EXPECT_FALSE(world.movePlayer(1, { 127, 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x_subcell, 0U);
    EXPECT_TRUE(world.movePlayer(1, { static_cast<uint8_t>(-127), 0 }));
    EXPECT_EQ(world.player(1)->x, 4U);
    EXPECT_EQ(
        world.player(1)->x_subcell,
        shared::SUBCELLS_PER_CELL - shared::MOVEMENT_SUBCELLS_PER_TICK
    );
}

TEST(WorldTest, DiagonalMoveStopsWhenBothAxesAreBlocked) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });
    world.spawnPlayer(3, '$', std::pair<uint8_t, uint8_t>{ 5, 7 });

    EXPECT_FALSE(world.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x_subcell, 0U);
    EXPECT_EQ(world.player(1)->y_subcell, 0U);
}

TEST(WorldTest, HighAndLowPlatformEdgesAllowTangentialSlide) {
    static constexpr uint16_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;

    shared::World high_edge;
    high_edge.spawnPlayer(
        1,
        '@',
        std::pair<uint8_t, uint8_t>{ shared::World::MAX_PLAYER_ORIGIN_CELL, 5 }
    );
    EXPECT_TRUE(high_edge.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(high_edge.player(1).has_value());
    EXPECT_EQ(high_edge.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(high_edge.player(1)->x_subcell, 0U);
    EXPECT_EQ(high_edge.player(1)->y_subcell, DIAGONAL_STEP);

    shared::World low_edge;
    low_edge.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 0 });
    EXPECT_TRUE(low_edge.movePlayer(1, { 127, static_cast<uint8_t>(-127) }));
    ASSERT_TRUE(low_edge.player(1).has_value());
    EXPECT_EQ(low_edge.player(1)->x_subcell, DIAGONAL_STEP);
    EXPECT_EQ(low_edge.player(1)->y, 0U);
    EXPECT_EQ(low_edge.player(1)->y_subcell, 0U);
}

TEST(WorldTest, MaxCornerStopsDiagonalMovement) {
    shared::World world;
    world.spawnPlayer(
        1,
        '@',
        std::pair<uint8_t, uint8_t>{
            shared::World::MAX_PLAYER_ORIGIN_CELL,
            shared::World::MAX_PLAYER_ORIGIN_CELL,
        }
    );

    EXPECT_FALSE(world.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(world.player(1)->y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(world.player(1)->x_subcell, 0U);
    EXPECT_EQ(world.player(1)->y_subcell, 0U);
}

TEST(WorldTest, DespawnRemovesOnlyRequestedPlayer) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 1, 1 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 10, 10 });
    world.despawnPlayer(1);
    EXPECT_FALSE(world.player(1).has_value());
    EXPECT_TRUE(world.player(2).has_value());
}
