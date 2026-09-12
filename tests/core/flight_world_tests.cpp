#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

TEST(FlightWorldTest, UsesFixedClearAirSpawnAndSignedCoordinates)
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
    EXPECT_EQ(world.player(1)->x, -1);
    EXPECT_EQ(world.player(1)->x_subcell, 6'400U);
    EXPECT_DOUBLE_EQ(shared::playerPositionX(*world.player(1)), -0.36);
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

TEST(FlightWorldTest, RejectsBoundsWithoutMutatingAuthoritativePosition)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    ASSERT_TRUE(world.setPlayerPosition(1, {
        .x = shared::World::FLIGHT_MIN_CELL,
        .y = 0,
        .z = 0,
    }));
    EXPECT_FALSE(world.movePlayer(1, { .x = static_cast<uint8_t>(-127), .y = 0, .z = 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::FLIGHT_MIN_CELL);
    EXPECT_EQ(world.player(1)->x_subcell, 0U);

    EXPECT_FALSE(world.setPlayerPosition(1, {
        .x = shared::World::FLIGHT_MAX_CELL + 1,
        .y = 0,
        .z = 0,
    }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(world.player(1)->x, shared::World::FLIGHT_MIN_CELL);
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
