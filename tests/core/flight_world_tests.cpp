#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

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

TEST(FlightWorldTest, NormalizesThreeAxisMovementWithoutGravityOrCollisions)
{
    static constexpr uint16_t THREE_AXIS_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 220U;

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    world.spawnPlayer(2, '#');
    ASSERT_TRUE(world.player(1).has_value());
    ASSERT_TRUE(world.player(2).has_value());
    EXPECT_EQ(world.player(1)->x, world.player(2)->x);
    EXPECT_EQ(world.player(1)->y, world.player(2)->y);
    EXPECT_EQ(world.player(1)->z, world.player(2)->z);

    ASSERT_TRUE(world.movePlayer(1, { .x = 127, .y = 127, .z = 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x_subcell, THREE_AXIS_STEP);
    EXPECT_EQ(world.player(1)->y_subcell, THREE_AXIS_STEP);
    EXPECT_EQ(world.player(1)->z_subcell, THREE_AXIS_STEP);
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
