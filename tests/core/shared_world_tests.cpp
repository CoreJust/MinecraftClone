#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename Value>
Value const& requiredValue(std::optional<Value> const& optional)
{
    if (optional.has_value()) {
        return *optional;
    }
    throw std::runtime_error("Test expected an optional value");
}

} // namespace

TEST(WorldTest, ExplicitSpawnAndLookup) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 3, 4 });
    auto const player = world.player(1);
    ASSERT_TRUE(player.has_value());
    EXPECT_EQ(requiredValue(player).x, 3u);
    EXPECT_EQ(requiredValue(player).y, 4u);
    EXPECT_TRUE(world.playerExists('@'));
    EXPECT_TRUE(world.playerByCharacter('@').has_value());
}

TEST(WorldTest, ReplicatedOverlapAcceptsOnlyDefinedMovementCapabilities) {
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', shared::PlayerPosition{ .x = 100, .y = 100, .z = 2'000 });
    shared::Player state{
        .id = 2U,
        .x = 100,
        .y = 100,
        .z = 2'000,
        .z_subcell = 1'000U,
        .ch = '#',
        .palette_index = 7U,
        .vertical_velocity_subcells = -2'500,
    };
    uint8_t last_accepted = 0U;
    for (uint16_t bits = 0U; bits <= 255U; ++bits) {
        SCOPED_TRACE(bits);
        state.movement_capabilities.bits = static_cast<uint8_t>(bits);
        bool const valid = bits == 0U || bits == 1U || bits == 3U;

        EXPECT_EQ(world.applyReplicatedPlayerState(state), valid);

        if (valid) {
            last_accepted = static_cast<uint8_t>(bits);
        }
        auto const replicated = world.player(2U);
        ASSERT_TRUE(replicated.has_value());
        EXPECT_EQ(replicated->movement_capabilities.bits, last_accepted);
        EXPECT_EQ(replicated->vertical_velocity_subcells, -2'500);
        EXPECT_EQ(replicated->z_subcell, 1'000U);
        EXPECT_EQ(replicated->palette_index, 7U);
        EXPECT_EQ(requiredValue(world.player(1U)).movement_capabilities.bits, 3U);
        EXPECT_EQ(world.players().size(), 2U);
    }
}

TEST(WorldTest, RequestedCapabilitiesStillRequireCollisionAdmissionAfterReplication) {
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', shared::PlayerPosition{ .x = 100, .y = 100, .z = 2'000 });
    ASSERT_TRUE(world.applyReplicatedPlayerState({
        .id = 2U,
        .x = 100,
        .y = 100,
        .z = 2'000,
        .ch = '#',
        .movement_capabilities = { .bits = 3U },
        .vertical_velocity_subcells = -2'500,
    }));

    EXPECT_FALSE(world.setPlayerMovementCapabilities(2U, { .bits = 0U }));
    EXPECT_FALSE(world.setPlayerMovementCapabilities(2U, { .bits = 1U }));
    EXPECT_EQ(requiredValue(world.player(2U)).movement_capabilities.bits, 3U);
    EXPECT_EQ(requiredValue(world.player(2U)).vertical_velocity_subcells, -2'500);

    ASSERT_TRUE(world.setPlayerPosition(1U, { .x = 104, .y = 100, .z = 2'000 }));
    EXPECT_TRUE(world.setPlayerMovementCapabilities(2U, { .bits = 0U }));
    EXPECT_TRUE(world.setPlayerMovementCapabilities(2U, { .bits = 1U }));
}

TEST(WorldTest, ReplicatedStateRejectsInvalidValuesAndIdentityWithoutMutation) {
    shared::Player const original{
        .id = 1U,
        .x = 100,
        .y = 100,
        .z = 2'000,
        .ch = '@',
        .palette_index = 7U,
        .vertical_velocity_subcells = -2'500,
    };
    shared::World world{ shared::WorldMode::Flight };
    ASSERT_TRUE(world.applyReplicatedPlayerState(original));
    std::array invalid{ original, original, original, original, original, original };
    invalid[0].palette_index = shared::PLAYER_PALETTE_COUNT;
    invalid[1].x = -1;
    invalid[2].z = shared::World::FLIGHT_ENTITY_MAX_Z + 1;
    invalid[3].x_subcell = shared::SUBCELLS_PER_CELL;
    invalid[4].ch = '#';
    invalid[5].id = 2U;
    for (auto const& state : invalid) {
        EXPECT_FALSE(world.applyReplicatedPlayerState(state));
        auto const replicated = world.player(1U);
        ASSERT_TRUE(replicated.has_value());
        EXPECT_EQ(replicated->ch, '@');
        EXPECT_EQ(replicated->x, 100);
        EXPECT_EQ(replicated->z, 2'000);
        EXPECT_EQ(replicated->x_subcell, 0U);
        EXPECT_EQ(replicated->palette_index, 7U);
        EXPECT_EQ(replicated->vertical_velocity_subcells, -2'500);
        EXPECT_EQ(world.players().size(), 1U);
    }
}

TEST(WorldTest, MovePlayerRejectsUnknownAndOccupiedPositions) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });
    EXPECT_FALSE(world.movePlayer(99, { 1, 0 }));
    EXPECT_FALSE(world.movePlayer(1, { 127, 0 }));
    EXPECT_EQ(requiredValue(world.player(1)).x, 5u);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 0u);
}

TEST(WorldTest, BorderCellsAreValidAndCollisionChecksDoNotUnderflow) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 0, 0 });
    EXPECT_TRUE(world.movePlayer(1, { 127, 0 }));
    EXPECT_EQ(requiredValue(world.player(1)).x, 0u);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_TRUE(world.movePlayer(1, { static_cast<uint8_t>(-127), 0 }));
    EXPECT_EQ(requiredValue(world.player(1)).x, 0u);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 0u);
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
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(world.player(1)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
}

TEST(WorldTest, RandomSpawnsStayWithinTheWorld) {
    shared::World world;
    for (shared::PlayerId id = 0; id < 8; ++id) {
        world.spawnPlayer(id, static_cast<char>('@' + id));
        ASSERT_TRUE(world.player(id).has_value());
        EXPECT_LE(requiredValue(world.player(id)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
        EXPECT_LE(requiredValue(world.player(id)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
        auto const spawned = world.player(id);
        for (auto const& other : world.players()) {
            if (other.id != id) {
                EXPECT_TRUE(
                    std::abs(static_cast<int32_t>(requiredValue(spawned).x) - other.x) > 1
                    || std::abs(static_cast<int32_t>(requiredValue(spawned).y) - other.y) > 1
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
    ASSERT_TRUE(horizontal.setPlayerPosition(1, LAST_INTERIOR_CELL, 4, CARDINAL_START_SUBCELL, 0));
    ASSERT_TRUE(horizontal.movePlayer(1, { POSITIVE_DIRECTION, 0 }));
    ASSERT_TRUE(horizontal.player(1).has_value());
    EXPECT_EQ(requiredValue(horizontal.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(horizontal.player(1)).x_subcell, 0U);
    EXPECT_FALSE(horizontal.movePlayer(1, { POSITIVE_DIRECTION, 0 }));
    EXPECT_EQ(requiredValue(horizontal.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(horizontal.player(1)).x_subcell, 0U);

    shared::World vertical;
    vertical.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 4, LAST_INTERIOR_CELL });
    ASSERT_TRUE(vertical.setPlayerPosition(1, 4, LAST_INTERIOR_CELL, 0, CARDINAL_START_SUBCELL));
    ASSERT_TRUE(vertical.movePlayer(1, { 0, POSITIVE_DIRECTION }));
    ASSERT_TRUE(vertical.player(1).has_value());
    EXPECT_EQ(requiredValue(vertical.player(1)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(vertical.player(1)).y_subcell, 0U);
    EXPECT_FALSE(vertical.movePlayer(1, { 0, POSITIVE_DIRECTION }));
    EXPECT_EQ(requiredValue(vertical.player(1)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(vertical.player(1)).y_subcell, 0U);

    shared::World diagonal;
    diagonal.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ LAST_INTERIOR_CELL, LAST_INTERIOR_CELL });
    ASSERT_TRUE(diagonal.setPlayerPosition(
        1,
        LAST_INTERIOR_CELL,
        LAST_INTERIOR_CELL,
        DIAGONAL_START_SUBCELL,
        DIAGONAL_START_SUBCELL
    ));
    ASSERT_TRUE(diagonal.movePlayer(1, { POSITIVE_DIRECTION, POSITIVE_DIRECTION }));
    ASSERT_TRUE(diagonal.player(1).has_value());
    EXPECT_EQ(requiredValue(diagonal.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(diagonal.player(1)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(diagonal.player(1)).x_subcell, 0U);
    EXPECT_EQ(requiredValue(diagonal.player(1)).y_subcell, 0U);
    EXPECT_FALSE(diagonal.movePlayer(1, { POSITIVE_DIRECTION, POSITIVE_DIRECTION }));
    EXPECT_EQ(requiredValue(diagonal.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(diagonal.player(1)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(diagonal.player(1)).x_subcell, 0U);
    EXPECT_EQ(requiredValue(diagonal.player(1)).y_subcell, 0U);
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
        EXPECT_EQ(requiredValue(world.player(2)).y, inside_y);
        EXPECT_EQ(requiredValue(world.player(2)).y_subcell, 0U);
    }
}

TEST(WorldTest, ZeroDirectionDoesNotMovePlayer) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    EXPECT_TRUE(world.movePlayer(1, { 0, 0 }));
    EXPECT_EQ(requiredValue(world.player(1)).x, 5u);
    EXPECT_EQ(requiredValue(world.player(1)).y, 5u);
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
    EXPECT_EQ(requiredValue(cardinal.player(1)).x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
    EXPECT_EQ(requiredValue(cardinal.player(1)).y_subcell, 0u);
    uint32_t const diagonal_distance_squared = static_cast<uint32_t>(requiredValue(diagonal.player(1)).x_subcell)
        * requiredValue(diagonal.player(1)).x_subcell + static_cast<uint32_t>(requiredValue(diagonal.player(1)).y_subcell)
        * requiredValue(diagonal.player(1)).y_subcell;
    EXPECT_LE(diagonal_distance_squared, static_cast<uint32_t>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        * shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(WorldTest, FullTickMovesAtTheConfiguredSpeedAndRejectsAnOccupiedEndpoint) {
    shared::World moving_world;
    moving_world.spawnPlayer(1U, '@', std::pair<uint8_t, uint8_t>{ 5U, 5U });

    ASSERT_TRUE(moving_world.movePlayer(1U, { 127U, 0U }));
    ASSERT_TRUE(moving_world.player(1U).has_value());
    EXPECT_EQ(requiredValue(moving_world.player(1U)).x, 5U);
    EXPECT_EQ(requiredValue(moving_world.player(1U)).x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);

    shared::World blocked_world;
    blocked_world.spawnPlayer(1U, '@', std::pair<uint8_t, uint8_t>{ 5U, 5U });
    blocked_world.spawnPlayer(2U, '#', std::pair<uint8_t, uint8_t>{ 7U, 5U });

    EXPECT_FALSE(blocked_world.movePlayer(1U, { 127U, 0U }));
    ASSERT_TRUE(blocked_world.player(1U).has_value());
    EXPECT_EQ(requiredValue(blocked_world.player(1U)).x, 5U);
    EXPECT_EQ(requiredValue(blocked_world.player(1U)).x_subcell, 0U);
}

TEST(WorldTest, SubcellCollisionRejectsOverlapBeforeEitherPlayerChangesCells) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });
    ASSERT_TRUE(world.setPlayerPosition(1, 5, 5, 5'000, 0));
    ASSERT_TRUE(world.setPlayerPosition(2, 7, 5, 5'000, 0));

    EXPECT_FALSE(world.movePlayer(1, { 127, 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x, 5U);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 5'000U);
}

TEST(WorldTest, DiagonalCornerResolvesXBeforeY) {
    static constexpr uint16_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;

    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 7 });

    EXPECT_TRUE(world.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, DIAGONAL_STEP);
    EXPECT_EQ(requiredValue(world.player(1)).y_subcell, 0U);
}

TEST(WorldTest, HorizontalAndVerticalGlancingCollisionsSlideWithoutSpeedGain) {
    static constexpr uint16_t DIAGONAL_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;

    shared::World horizontal_blocker;
    horizontal_blocker.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    horizontal_blocker.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });

    EXPECT_TRUE(horizontal_blocker.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(horizontal_blocker.player(1).has_value());
    EXPECT_EQ(requiredValue(horizontal_blocker.player(1)).x_subcell, 0U);
    EXPECT_EQ(requiredValue(horizontal_blocker.player(1)).y_subcell, DIAGONAL_STEP);

    shared::World vertical_blocker;
    vertical_blocker.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    vertical_blocker.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 5, 7 });

    EXPECT_TRUE(vertical_blocker.movePlayer(1, { 127, 127 }));
    ASSERT_TRUE(vertical_blocker.player(1).has_value());
    EXPECT_EQ(requiredValue(vertical_blocker.player(1)).x_subcell, DIAGONAL_STEP);
    EXPECT_EQ(requiredValue(vertical_blocker.player(1)).y_subcell, 0U);
}

TEST(WorldTest, BlockedStraightMoveCanMoveAway) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 5 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 7, 5 });

    EXPECT_FALSE(world.movePlayer(1, { 127, 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 0U);
    EXPECT_TRUE(world.movePlayer(1, { static_cast<uint8_t>(-127), 0 }));
    EXPECT_EQ(requiredValue(world.player(1)).x, 4U);
    EXPECT_EQ(
        requiredValue(world.player(1)).x_subcell,
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
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 0U);
    EXPECT_EQ(requiredValue(world.player(1)).y_subcell, 0U);
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
    EXPECT_EQ(requiredValue(high_edge.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(high_edge.player(1)).x_subcell, 0U);
    EXPECT_EQ(requiredValue(high_edge.player(1)).y_subcell, DIAGONAL_STEP);

    shared::World low_edge;
    low_edge.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 5, 0 });
    EXPECT_TRUE(low_edge.movePlayer(1, { 127, static_cast<uint8_t>(-127) }));
    ASSERT_TRUE(low_edge.player(1).has_value());
    EXPECT_EQ(requiredValue(low_edge.player(1)).x_subcell, DIAGONAL_STEP);
    EXPECT_EQ(requiredValue(low_edge.player(1)).y, 0U);
    EXPECT_EQ(requiredValue(low_edge.player(1)).y_subcell, 0U);
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
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(world.player(1)).y, shared::World::MAX_PLAYER_ORIGIN_CELL);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 0U);
    EXPECT_EQ(requiredValue(world.player(1)).y_subcell, 0U);
}

TEST(WorldTest, DespawnRemovesOnlyRequestedPlayer) {
    shared::World world;
    world.spawnPlayer(1, '@', std::pair<uint8_t, uint8_t>{ 1, 1 });
    world.spawnPlayer(2, '#', std::pair<uint8_t, uint8_t>{ 10, 10 });
    world.despawnPlayer(1);
    EXPECT_FALSE(world.player(1).has_value());
    EXPECT_TRUE(world.player(2).has_value());
}
