#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <gtest/gtest.h>

#include <algorithm>

TEST(FlightWorldTest, UsesFirstWaveSpawnAndWrappedCoordinates)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::FLIGHT_SPAWN.x);
    EXPECT_EQ(world.player(1)->y, shared::World::FLIGHT_SPAWN.y);
    EXPECT_EQ(world.player(1)->z, shared::World::FLIGHT_SPAWN.z);

    ASSERT_TRUE(world.setPlayerPosition(1, {
        .x = 0,
        .y = 0,
        .z = 0,
        .x_subcell = 2'000,
    }));
    ASSERT_TRUE(world.movePlayer(1, { .x = static_cast<uint8_t>(-127), .y = 0, .z = 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::FLIGHT_MAX_CELL);
    EXPECT_EQ(world.player(1)->x_subcell, 6'400U);
    EXPECT_DOUBLE_EQ(shared::playerPositionX(*world.player(1)), 65'535.64);
}

TEST(FlightWorldTest, NormalizesThreeAxisMovementWithoutCollisionBypass)
{
    static constexpr uint16_t THREE_AXIS_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 220U;

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    world.spawnPlayer(2, '#');
    ASSERT_TRUE(world.player(1).has_value());
    ASSERT_TRUE(world.player(2).has_value());
    EXPECT_NE(world.player(1)->x, world.player(2)->x);
    EXPECT_EQ(world.player(1)->y, world.player(2)->y);
    EXPECT_EQ(world.player(1)->z, world.player(2)->z);

    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    ASSERT_TRUE(world.movePlayer(1, { .x = 127, .y = 127, .z = 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x_subcell, THREE_AXIS_STEP);
    EXPECT_EQ(world.player(1)->y_subcell, THREE_AXIS_STEP);
    EXPECT_EQ(world.player(1)->z_subcell, THREE_AXIS_STEP);
}

TEST(FlightWorldTest, FlightCollisionIgnoresPlayersAtNonOverlappingHeights)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 800 });
    world.spawnPlayer(2U, '#', { .x = 100, .y = 100, .z = 803 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U, .z = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(world.player(1U)->x_subcell, 0U);
}

TEST(FlightWorldTest, FlightCollisionSlidesAlongAnAdjacentPlayerWall)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 800 });
    world.spawnPlayer(2U, '#', {
        .x = 100,
        .y = 100,
        .z = 800,
        .x_subcell = shared::World::PLAYER_WIDTH_SUBCELLS,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(world.player(1U)->x_subcell, 0U);
    EXPECT_GT(world.player(1U)->y_subcell, 0U);
}

TEST(FlightWorldTest, CollisionModeStopsVerticalMovementAtAnotherPlayer)
{
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(100, 100);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = terrain_height });
    world.spawnPlayer(2U, '#', { .x = 100, .y = 100, .z = terrain_height + 2 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_FALSE(world.movePlayer(1U, { .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(shared::playerPositionZ(*world.player(1U)), terrain_height + 2.0);
    EXPECT_EQ(world.player(1U)->vertical_velocity_subcells, 0);
}

TEST(FlightWorldTest, CollisionModeTreatsPlayerBodiesAcrossTheWrapSeamAsAdjacent)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = shared::World::FLIGHT_MAX_CELL,
        .y = 100,
        .z = 800,
        .x_subcell = 9'000U,
    });
    world.spawnPlayer(2U, '#', {
        .x = 0,
        .y = 100,
        .z = 800,
        .x_subcell = shared::World::PLAYER_WIDTH_SUBCELLS,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.player(1U).has_value());
    shared::Player const before = *world.player(1U);
    EXPECT_FALSE(world.movePlayer(1U, { .x = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(world.player(1U)->x, before.x);
    EXPECT_EQ(world.player(1U)->x_subcell, before.x_subcell);
}

TEST(FlightWorldTest, NonFlightPlayerFalls)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 500 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    ASSERT_TRUE(world.movePlayer(1U, {}));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(world.player(1U)->z, 500);
}

TEST(FlightWorldTest, NonFlightPlayerLandsOnTerrainInsteadOfRemainingSuspended)
{
    static constexpr int32_t X = 100;
    static constexpr int32_t Y = 100;
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(X, Y);

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = X,
        .y = Y,
        .z = terrain_height + 1,
        .z_subcell = 1U,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    bool moved = false;
    for (uint8_t tick = 0U; tick < 64U; ++tick) {
        if (!world.movePlayer(1U, {})) {
            break;
        }
        moved = true;
    }
    EXPECT_TRUE(moved);
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(world.player(1U)->z, terrain_height);
    EXPECT_EQ(world.player(1U)->z_subcell, 0U);
}

TEST(FlightWorldTest, FlightWithoutCollisionBypassCannotEnterTerrain)
{
    static constexpr int32_t X = 100;
    static constexpr int32_t Y = 100;
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(X, Y);

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = X,
        .y = Y,
        .z = terrain_height + 1,
        .z_subcell = 1U,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    bool moved = false;
    for (uint8_t tick = 0U; tick < 64U; ++tick) {
        if (!world.movePlayer(1U, { .z = static_cast<uint8_t>(-127) })) {
            break;
        }
        moved = true;
    }
    EXPECT_TRUE(moved);
    EXPECT_FALSE(world.movePlayer(1U, { .z = static_cast<uint8_t>(-127) }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GE(world.player(1U)->z, terrain_height);
}

TEST(FlightWorldTest, CollisionUsesTheWholePlayerFootprintAcrossAdjacentColumns)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 32'767,
        .y = 32'768,
        .z = 805,
        .x_subcell = 0U,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.player(1U).has_value());
    shared::Player const before = *world.player(1U);
    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(world.player(1U)->x, before.x);
    EXPECT_GT(world.player(1U)->x_subcell, before.x_subcell);
    EXPECT_LT(world.player(1U)->x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
}

TEST(FlightWorldTest, AcceleratedFlightSweepsTerrainInsteadOfTunnelingThroughRidge)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 32'700,
        .y = 32'768,
        .z = 100,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.player(1U).has_value());
    shared::Player const before = *world.player(1U);
    EXPECT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(world.player(1U)->x, before.x);
    EXPECT_LT(world.player(1U)->x, before.x + 112);
}

TEST(FlightWorldTest, AcceleratedFlightCanClimbAfterAdvancingAcrossTerrainStep)
{
    shared::TerrainGenerator const terrain;
    static constexpr int32_t X = 29'000;
    static constexpr int32_t Y = 32'185;
    uint16_t const terrain_height = terrain.heightAt(X, Y);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = X, .y = Y, .z = terrain_height });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    EXPECT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .y = 127U,
        .z = 127U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(*world.player(1U)), static_cast<double>(terrain_height));
}

TEST(FlightWorldTest, CollisionSweepsDescendingDiagonalFlightThroughAnotherBody)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 1'000 });
    world.spawnPlayer(2U, '#', { .x = 150, .y = 100, .z = 950 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .z = static_cast<uint8_t>(-127),
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(world.player(1U)->x, 200);
}

TEST(FlightWorldTest, CollisionSweepsDiagonalFlightAroundCornerInsteadOfBypassingBody)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 800 });
    world.spawnPlayer(2U, '#', { .x = 150, .y = 150, .z = 800 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.player(1U).has_value());
    shared::Player const before = *world.player(1U);
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .y = 127U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    double const before_x = shared::playerPositionX(before);
    double const before_y = shared::playerPositionY(before);
    double const after_x = shared::playerPositionX(*world.player(1U));
    double const after_y = shared::playerPositionY(*world.player(1U));
    EXPECT_TRUE(after_x == before_x || after_y == before_y);
}

TEST(FlightWorldTest, CollisionSweepsDescendingDiagonalFlightAcrossWrappedSeam)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = shared::World::FLIGHT_MAX_CELL,
        .y = 100,
        .z = 1'000,
    });
    world.spawnPlayer(2U, '#', { .x = 2, .y = 100, .z = 998 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .z = static_cast<uint8_t>(-127),
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(world.player(1U)->x, 10);
}

TEST(FlightWorldTest, CollisionSweepsDiagonalFlightAroundCornerAcrossWrappedSeam)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = shared::World::FLIGHT_MAX_CELL,
        .y = 100,
        .z = 800,
    });
    world.spawnPlayer(2U, '#', { .x = 20, .y = 121, .z = 800 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.player(1U).has_value());
    shared::Player const before = *world.player(1U);
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .y = 127U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    double const before_x = shared::playerPositionX(before);
    double const before_y = shared::playerPositionY(before);
    double const after_x = shared::playerPositionX(*world.player(1U));
    double const after_y = shared::playerPositionY(*world.player(1U));
    EXPECT_TRUE(after_x == before_x || after_y == before_y);
}

TEST(FlightWorldTest, NonFlightSpaceJumpsAndClearsAdjacentTerrainStep)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 32'767,
        .y = 32'768,
        .z = 805,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_TRUE(world.movePlayer(1U, { .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(*world.player(1U)), 805.0);

    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionX(*world.player(1U)), 32'767.5);
}

TEST(FlightWorldTest, NonFlightJumpUsesAStableAirborneCurve)
{
    static constexpr int32_t X = 100;
    static constexpr int32_t Y = 100;
    static constexpr uint8_t TICKS_TO_APEX = 7U;
    static constexpr int32_t FIRST_TICK_VERTICAL_VELOCITY = 5'250;
    static constexpr double FIRST_TICK_HEIGHT_DELTA = 0.6125;
    static constexpr double MAXIMUM_HEIGHT_DELTA = 1.4;
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(X, Y);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = X, .y = Y, .z = terrain_height });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    ASSERT_TRUE(world.movePlayer(1U, { .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    double const first_height = shared::playerPositionZ(*world.player(1U));
    EXPECT_DOUBLE_EQ(first_height, static_cast<double>(terrain_height) + FIRST_TICK_HEIGHT_DELTA);
    EXPECT_EQ(world.player(1U)->vertical_velocity_subcells, FIRST_TICK_VERTICAL_VELOCITY);

    double maximum_height = first_height;
    for (uint8_t tick = 0U; tick < TICKS_TO_APEX; ++tick) {
        ASSERT_TRUE(world.movePlayer(1U, {}));
        ASSERT_TRUE(world.player(1U).has_value());
        maximum_height = std::max(maximum_height, shared::playerPositionZ(*world.player(1U)));
    }
    EXPECT_NEAR(maximum_height, static_cast<double>(terrain_height) + MAXIMUM_HEIGHT_DELTA, 1e-9);
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(*world.player(1U)), static_cast<double>(terrain_height));
}

TEST(FlightWorldTest, NonFlightMovementSlidesAlongAAdjacentTerrainWall)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 32'767,
        .y = 32'768,
        .z = 805,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_TRUE(world.movePlayer(1U, { .y = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionY(*world.player(1U)), 32'768.5);
}

TEST(FlightWorldTest, NonFlightMovementUsesFlightWorldCoordinates)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 35'050,
        .y = 32'768,
        .z = 800,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionX(*world.player(1U)), 35'050.5);
}

TEST(FlightWorldTest, AcceleratesAllFlightAxesFivefold)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    ASSERT_TRUE(world.setPlayerPosition(1, { .x = 100, .y = 100, .z = 100 }));

    ASSERT_TRUE(world.movePlayer(1, {
        .x = 127,
        .y = 127,
        .z = 127,
        .accelerated = true,
    }));

    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, 101);
    EXPECT_EQ(world.player(1)->y, 101);
    EXPECT_EQ(world.player(1)->z, 101);
    EXPECT_EQ(world.player(1)->x_subcell, 6'160U);
    EXPECT_EQ(world.player(1)->y_subcell, 6'160U);
    EXPECT_EQ(world.player(1)->z_subcell, 6'160U);
}

TEST(FlightWorldTest, UsesSelectedAccelerationProfile)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    ASSERT_TRUE(world.setPlayerPosition(1, { .x = 100, .y = 100, .z = 100 }));

    ASSERT_TRUE(world.movePlayer(1, {
        .x = 127,
        .y = 127,
        .z = 127,
        .accelerated = true,
        .speedup = 80U,
    }));

    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, 125);
    EXPECT_EQ(world.player(1)->y, 125);
    EXPECT_EQ(world.player(1)->z, 125);
    EXPECT_EQ(world.player(1)->x_subcell, 8'560U);
    EXPECT_EQ(world.player(1)->y_subcell, 8'560U);
    EXPECT_EQ(world.player(1)->z_subcell, 8'560U);
}

TEST(FlightWorldTest, SupportsExtremeAccelerationProfiles)
{
    EXPECT_TRUE(shared::isFlightSpeedupProfile(200U));
    EXPECT_TRUE(shared::isFlightSpeedupProfile(500U));
}

TEST(FlightWorldTest, AuthoritativeCapabilitiesAllowOnlyTheThreeValidCombinations)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 800 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_FALSE(world.setPlayerMovementCapabilities(1U, { .bits = 2U }));
    EXPECT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 0U }));
    EXPECT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    EXPECT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 3U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(world.player(1U)->movement_capabilities.bits, 3U);
}

TEST(FlightWorldTest, FlightIsGrantedPerPlayerRatherThanByClientIntent)
{
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(4, 4);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = terrain_height });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_TRUE(world.movePlayer(1U, { .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(*world.player(1U)), terrain_height);

    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    EXPECT_TRUE(world.movePlayer(1U, { .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(*world.player(1U)), terrain_height);
}

TEST(FlightWorldTest, WrapsHorizontalBoundsAndRejectsVerticalBounds)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    ASSERT_TRUE(world.setPlayerPosition(1, {
        .x = shared::World::FLIGHT_MIN_CELL,
        .y = 0,
        .z = 0,
    }));
    EXPECT_TRUE(world.movePlayer(1, { .x = static_cast<uint8_t>(-127), .y = 0, .z = 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::FLIGHT_MAX_CELL);
    EXPECT_EQ(world.player(1)->x_subcell, 4'400U);

    EXPECT_FALSE(world.setPlayerPosition(1, {
        .x = shared::World::FLIGHT_MIN_CELL,
        .y = 0,
        .z = shared::World::FLIGHT_MAX_Z + 1,
    }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::FLIGHT_MAX_CELL);
}

TEST(FlightWorldTest, CanonicalConfigurationIdentifiesTheSeededChunk)
{
    shared::WorldConfiguration const configuration = shared::World::canonicalConfiguration();
    shared::Chunk const chunk = shared::Chunk::makeStoneFixture({}, configuration.seed);

    EXPECT_TRUE(shared::isValidWorldConfiguration(configuration));
    EXPECT_EQ(configuration.chunk_width, shared::Chunk::SIDE_LENGTH);
    EXPECT_EQ(configuration.chunk_height, shared::Chunk::SIDE_LENGTH);
    EXPECT_EQ(configuration.chunk_depth, shared::Chunk::SIDE_LENGTH);
    EXPECT_EQ(configuration.chunk_content_digest, chunk.contentIdentity().content_hash);
}
