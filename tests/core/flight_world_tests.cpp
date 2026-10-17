#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
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

TEST(FlightWorldTest, UsesFirstWaveSpawnAndWrappedCoordinates)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::FLIGHT_SPAWN.x);
    EXPECT_EQ(requiredValue(world.player(1)).y, shared::World::FLIGHT_SPAWN.y);
    EXPECT_EQ(requiredValue(world.player(1)).z, shared::World::FLIGHT_SPAWN.z);

    ASSERT_TRUE(world.setPlayerPosition(1, {
        .x = 0,
        .y = 0,
        .z = 0,
        .x_subcell = 2'000,
    }));
    ASSERT_TRUE(world.movePlayer(1, { .x = static_cast<uint8_t>(-127), .y = 0, .z = 0 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::FLIGHT_MAX_CELL);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 6'400U);
    EXPECT_DOUBLE_EQ(shared::playerPositionX(requiredValue(world.player(1))), 65'535.64);
}

TEST(FlightWorldTest, ForecastMatchesFlightMovementWithoutChangingSourcePlayer)
{
    static constexpr std::array<shared::Direction, 6U> DIRECTIONS{
        shared::Direction{ .x = 127U, .y = 0U, .accelerated = true, .speedup = 200U },
        shared::Direction{ .x = 127U, .y = 127U, .accelerated = true, .speedup = 200U },
        shared::Direction{ .x = 0U, .y = 0U, .z = 127U, .accelerated = true, .speedup = 200U },
        shared::Direction{ .x = 127U, .y = 127U, .z = 127U, .accelerated = true, .speedup = 200U },
        shared::Direction{ .x = 127U, .y = 127U, .z = 127U, .accelerated = false, .speedup = 200U },
        shared::Direction{ .x = static_cast<uint8_t>(-127), .y = 0U, .accelerated = true, .speedup = 200U },
    };
    static constexpr std::array<shared::PlayerPosition, 4U> POSITIONS{
        shared::PlayerPosition{ .x = 100, .y = 100, .z = 800 },
        shared::PlayerPosition{ .x = 15, .y = 31, .z = 800, .x_subcell = 9'999U, .y_subcell = 9'999U },
        shared::PlayerPosition{ .x = shared::World::FLIGHT_MAX_CELL, .y = 0, .z = 800, .x_subcell = 9'999U },
        shared::PlayerPosition{ .x = 0, .y = 0, .z = 800, .x_subcell = 2U },
    };
    static constexpr std::array<std::chrono::milliseconds, 3U> DURATIONS{
        shared::TICK, std::chrono::milliseconds{ 37 }, std::chrono::milliseconds{ 0 },
    };
    for (shared::PlayerPosition const& position : POSITIONS) {
        for (shared::Direction const direction : DIRECTIONS) {
            for (std::chrono::milliseconds const elapsed : DURATIONS) {
                shared::World world{ shared::WorldMode::Flight };
                world.spawnPlayer(1U, '@', position);
                shared::Player const before = requiredValue(world.player(1U));
                shared::World const& read_only = world;
                auto const forecast = read_only.forecastFlightPlayerPosition(1U, direction, elapsed);
                ASSERT_TRUE(forecast.has_value());
                shared::Player const unchanged = requiredValue(world.player(1U));
                EXPECT_EQ(unchanged.x, before.x);
                EXPECT_EQ(unchanged.y, before.y);
                EXPECT_EQ(unchanged.z, before.z);
                EXPECT_EQ(unchanged.x_subcell, before.x_subcell);
                EXPECT_EQ(unchanged.y_subcell, before.y_subcell);
                EXPECT_EQ(unchanged.z_subcell, before.z_subcell);
                EXPECT_EQ(unchanged.id, before.id);
                EXPECT_EQ(unchanged.ch, before.ch);
                EXPECT_EQ(unchanged.palette_index, before.palette_index);
                EXPECT_EQ(unchanged.movement_capabilities, before.movement_capabilities);
                EXPECT_EQ(unchanged.vertical_velocity_subcells, before.vertical_velocity_subcells);
                static_cast<void>(world.movePlayer(1U, direction, elapsed));
                shared::Player const actual = requiredValue(world.player(1U));
                EXPECT_EQ(forecast->x, actual.x);
                EXPECT_EQ(forecast->y, actual.y);
                EXPECT_EQ(forecast->z, actual.z);
                EXPECT_EQ(forecast->x_subcell, actual.x_subcell);
                EXPECT_EQ(forecast->y_subcell, actual.y_subcell);
                EXPECT_EQ(forecast->z_subcell, actual.z_subcell);
            }
        }
    }
}

TEST(FlightWorldTest, ForecastRejectsInvalidElapsedAndUnknownPlayer)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@');
    EXPECT_FALSE(world.forecastFlightPlayerPosition(2U, { .x = 127U, .y = 0U }));
    EXPECT_FALSE(world.forecastFlightPlayerPosition(1U, {}, std::chrono::milliseconds{ -1 }));
    EXPECT_FALSE(world.forecastFlightPlayerPosition(1U, {}, shared::TICK + std::chrono::milliseconds{ 1 }));
}

TEST(FlightWorldTest, ForecastRejectsNonFlightWorldAndTracksCurrentCapabilities)
{
    shared::World flat;
    flat.spawnPlayer(1U, '@');
    EXPECT_FALSE(flat.forecastFlightPlayerPosition(1U, { .x = 127U, .y = 0U }));
    shared::World flight{ shared::WorldMode::Flight };
    flight.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 800 });
    EXPECT_TRUE(flight.forecastFlightPlayerPosition(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(flight.setPlayerMovementCapabilities(1U, {}));
    EXPECT_FALSE(flight.forecastFlightPlayerPosition(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(flight.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    EXPECT_TRUE(flight.forecastFlightPlayerPosition(1U, { .x = 127U, .y = 0U }));
}

TEST(FlightWorldTest, ForecastRejectsOneShotCapabilityCycle)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@');
    shared::MovementCapabilities const before = requiredValue(world.player(1U)).movement_capabilities;
    EXPECT_FALSE(world.forecastFlightPlayerPosition(1U, {
        .x = 127U, .y = 0U, .cycle_movement_capabilities = true,
    }));
    EXPECT_EQ(requiredValue(world.player(1U)).movement_capabilities, before);
}

TEST(FlightWorldTest, CollisionMaterializationCandidatesIncludeBothSidesOfHorizontalSeams)
{
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = shared::World::FLIGHT_MAX_CELL, .y = 100, .z = 800});
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));

    std::vector<shared::ChunkCoordinate> const chunks = world.flightCollisionChunks(
        1U,
        {.x = 127U, .y = 0U, .z = 0U}
    );

    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == shared::World::FLIGHT_MAX_CELL / shared::Chunk::SIDE_LENGTH;
    }));
    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == 0;
    }));
}

TEST(FlightWorldTest, MaterializedBlocksAreCollisionAuthority)
{
    shared::SparseWorld blocks;
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 100, .y = 100, .z = 800});
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
    world.setCollisionWorld(&blocks);
    EXPECT_FALSE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    shared::HeightTile preview{.coordinate = {.x = 6, .y = 6}};
    preview.heights.fill(64U);
    ASSERT_TRUE(blocks.publishPreview(
        {.x = 6, .y = 6, .z = 50},
        preview,
        blocks.options().revision,
        blocks.options().seed
    ));
    EXPECT_EQ(blocks.queryBlock({100, 100, 800}).state, shared::GenerationState::Preview);
    EXPECT_FALSE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    shared::Chunk const air{{.x = 6, .y = 6, .z = 50}};
    ASSERT_TRUE(blocks.publishMaterializedChunk(air, blocks.options().revision, blocks.options().seed));
    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(world.setPlayerPosition(1U, {.x = 100, .y = 100, .z = 800}));
    ASSERT_TRUE(blocks.setBlock({.x = 101, .y = 100, .z = 800}, shared::Block::Stone));
    static_cast<void>(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(requiredValue(world.player(1U)).x, 100);
    EXPECT_LT(requiredValue(world.player(1U)).x_subcell, 3'750U);
}

TEST(FlightWorldTest, OnlyCollisionBypassSkipsMaterializationCandidates)
{
    static constexpr std::array<uint8_t, 3> VALID_CAPABILITY_BITS{0U, 1U, 3U};
    shared::SparseWorld unknown_world;

    for (uint8_t const capability_bits : VALID_CAPABILITY_BITS) {
        shared::World world{shared::WorldMode::Flight};
        world.spawnPlayer(1U, '@', {.x = 100, .y = 100, .z = 800});
        ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = capability_bits}));
        world.setCollisionWorld(&unknown_world);

        std::vector<shared::ChunkCoordinate> const candidates = world.flightCollisionChunks(
            1U,
            { .x = 127U, .y = 0U }
        );

        if (capability_bits == 3U) {
            EXPECT_TRUE(candidates.empty());
            continue;
        }
        ASSERT_FALSE(candidates.empty()) << "capability bits: " << static_cast<uint32_t>(capability_bits);
        for (shared::ChunkCoordinate const coordinate : candidates) {
            EXPECT_EQ(unknown_world.queryBlock({
                .x = static_cast<int64_t>(coordinate.x) * shared::Chunk::SIDE_LENGTH,
                .y = static_cast<int64_t>(coordinate.y) * shared::Chunk::SIDE_LENGTH,
                .z = static_cast<int64_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH,
            }).state, shared::GenerationState::Unknown);
        }
    }
}

TEST(FlightWorldTest, MaterializedTerrainSupportsLandingAndJumping)
{
    static constexpr uint8_t TERRAIN_HEIGHT = 8U;
    static constexpr uint8_t FALL_TICKS = 32U;
    shared::SparseWorld collision_world;
    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    for (uint8_t block_z = 0U; block_z < TERRAIN_HEIGHT; ++block_z) {
        uint32_t const block_index = static_cast<uint32_t>(block_z)
                * shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH
            + 4U * shared::Chunk::SIDE_LENGTH + 4U;
        blocks[block_index] = shared::Block::Stone;
    }
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 0 }, blocks },
        collision_world.options().revision,
        collision_world.options().seed
    ));

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 16 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));
    world.setCollisionWorld(&collision_world);

    for (uint8_t tick = 0U; tick < FALL_TICKS; ++tick) {
        static_cast<void>(world.movePlayer(1U, { .x = 0U, .y = 0U }));
    }

    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), TERRAIN_HEIGHT);
    EXPECT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_DOUBLE_EQ(
        shared::playerPositionZ(requiredValue(world.player(1U))),
        static_cast<double>(TERRAIN_HEIGHT) + 0.6125
    );
}

TEST(FlightWorldTest, MaterializedSupportUsesTheHighestColumnUnderThePlayerFootprint)
{
    static constexpr uint8_t FALL_TICKS = 32U;
    shared::SparseWorld collision_world;
    for (int32_t chunk_z = 1; chunk_z <= 2; ++chunk_z) {
        ASSERT_TRUE(collision_world.publishMaterializedChunk(
            shared::Chunk{ { .x = 0, .y = 0, .z = chunk_z } },
            collision_world.options().revision,
            collision_world.options().seed
        ));
    }
    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    for (uint8_t block_z = 0U; block_z < 5U; ++block_z) {
        uint32_t const block_index = static_cast<uint32_t>(block_z)
                * shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH
            + 4U * shared::Chunk::SIDE_LENGTH + 4U;
        blocks[block_index] = shared::Block::Stone;
    }
    for (uint8_t block_z = 0U; block_z < 16U; ++block_z) {
        uint32_t const block_index = static_cast<uint32_t>(block_z)
                * shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH
            + 4U * shared::Chunk::SIDE_LENGTH + 5U;
        blocks[block_index] = shared::Block::Stone;
    }
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 0 }, blocks },
        collision_world.options().revision,
        collision_world.options().seed
    ));

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 4,
        .y = 4,
        .z = 20,
        .x_subcell = 7'500U,
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));
    world.setCollisionWorld(&collision_world);

    for (uint8_t tick = 0U; tick < FALL_TICKS; ++tick) {
        static_cast<void>(world.movePlayer(1U, { .x = 0U, .y = 0U }));
    }

    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 16.0);
}

TEST(FlightWorldTest, MaterializationCandidatesCoverSweptBodyHeight)
{
    shared::SparseWorld blocks;
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 100, .y = 100, .z = 15});
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
    world.setCollisionWorld(&blocks);

    std::vector<shared::ChunkCoordinate> const chunks = world.flightCollisionChunks(
        1U, { .x = 0U, .y = 0U, .z = 127U }
    );
    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == 6 && coordinate.y == 6 && coordinate.z == 0;
    }));
    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == 6 && coordinate.y == 6 && coordinate.z == 1;
    }));
}

TEST(FlightWorldTest, AirborneJumpInputIncludesGravityCrossingIntoLowerChunk)
{
    shared::SparseWorld blocks;
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 100, .y = 100, .z = 16});
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));
    world.setCollisionWorld(&blocks);

    std::vector<shared::ChunkCoordinate> const chunks = world.flightCollisionChunks(
        1U, { .x = 0U, .y = 0U, .z = 127U }
    );
    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == 6 && coordinate.y == 6 && coordinate.z == 0;
    }));
    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == 6 && coordinate.y == 6 && coordinate.z == 1;
    }));
}

TEST(FlightWorldTest, NonFlightJumpInputIncludesHorizontalBoundaryCrossing)
{
    shared::SparseWorld blocks;
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 15, .y = 100, .z = 16, .x_subcell = 1'000U});
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));
    world.setCollisionWorld(&blocks);

    std::vector<shared::ChunkCoordinate> const chunks = world.flightCollisionChunks(
        1U, {.x = 127U, .y = 0U, .z = 127U}
    );
    EXPECT_TRUE(std::ranges::any_of(chunks, [](shared::ChunkCoordinate const coordinate) {
        return coordinate.x == 1 && coordinate.y == 6 && coordinate.z == 1;
    }));
}

TEST(FlightWorldTest, NormalizesThreeAxisMovementWithoutCollisionBypass)
{
    static constexpr uint16_t THREE_AXIS_STEP = shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 220U;

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1, '@');
    world.spawnPlayer(2, '#');
    ASSERT_TRUE(world.player(1).has_value());
    ASSERT_TRUE(world.player(2).has_value());
    EXPECT_NE(requiredValue(world.player(1)).x, requiredValue(world.player(2)).x);
    EXPECT_EQ(requiredValue(world.player(1)).y, requiredValue(world.player(2)).y);
    EXPECT_EQ(requiredValue(world.player(1)).z, requiredValue(world.player(2)).z);

    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    ASSERT_TRUE(world.movePlayer(1, { .x = 127, .y = 127, .z = 127 }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, THREE_AXIS_STEP);
    EXPECT_EQ(requiredValue(world.player(1)).y_subcell, THREE_AXIS_STEP);
    EXPECT_EQ(requiredValue(world.player(1)).z_subcell, THREE_AXIS_STEP);
}

TEST(FlightWorldTest, FlightCollisionIgnoresPlayersAtNonOverlappingHeights)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 800 });
    world.spawnPlayer(2U, '#', { .x = 100, .y = 100, .z = 803 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U, .z = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(requiredValue(world.player(1U)).x_subcell, 0U);
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
    EXPECT_EQ(requiredValue(world.player(1U)).x_subcell, 0U);
    EXPECT_GT(requiredValue(world.player(1U)).y_subcell, 0U);
}

TEST(FlightWorldTest, CollisionModeStopsVerticalMovementAtAnotherPlayer)
{
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(100, 100);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = terrain_height });
    world.spawnPlayer(2U, '#', { .x = 100, .y = 100, .z = terrain_height + 2 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_FALSE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(shared::playerPositionZ(requiredValue(world.player(1U))), terrain_height + 2.0);
    EXPECT_EQ(requiredValue(world.player(1U)).vertical_velocity_subcells, 0);
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
    shared::Player const before = requiredValue(world.player(1U));
    EXPECT_FALSE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(requiredValue(world.player(1U)).x, before.x);
    EXPECT_EQ(requiredValue(world.player(1U)).x_subcell, before.x_subcell);
}

TEST(FlightWorldTest, NonFlightPlayerFalls)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 500 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    ASSERT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(requiredValue(world.player(1U)).z, 500);
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
        if (!world.movePlayer(1U, { .x = 0U, .y = 0U })) {
            break;
        }
        moved = true;
    }
    EXPECT_TRUE(moved);
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(requiredValue(world.player(1U)).z, terrain_height);
    EXPECT_EQ(requiredValue(world.player(1U)).z_subcell, 0U);
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
        if (!world.movePlayer(1U, { .x = 0U, .y = 0U, .z = static_cast<uint8_t>(-127) })) {
            break;
        }
        moved = true;
    }
    EXPECT_TRUE(moved);
    EXPECT_FALSE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = static_cast<uint8_t>(-127) }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GE(requiredValue(world.player(1U)).z, terrain_height);
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
    shared::Player const before = requiredValue(world.player(1U));
    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(requiredValue(world.player(1U)).x, before.x);
    EXPECT_GT(requiredValue(world.player(1U)).x_subcell, before.x_subcell);
    EXPECT_LT(requiredValue(world.player(1U)).x_subcell, shared::MOVEMENT_SUBCELLS_PER_TICK);
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
    shared::Player const before = requiredValue(world.player(1U));
    EXPECT_TRUE(world.movePlayer(1U, {
        .x = 127U, .y = 0U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(requiredValue(world.player(1U)).x, before.x);
    EXPECT_LT(requiredValue(world.player(1U)).x, before.x + 112);
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
    EXPECT_GT(shared::playerPositionZ(requiredValue(world.player(1U))), static_cast<double>(terrain_height));
}

TEST(FlightWorldTest, CollisionSweepsDescendingDiagonalFlightThroughAnotherBody)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 1'000 });
    world.spawnPlayer(2U, '#', { .x = 150, .y = 100, .z = 950 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U, .y = 0U,
        .z = static_cast<uint8_t>(-127),
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(requiredValue(world.player(1U)).x, 200);
}

TEST(FlightWorldTest, CollisionSweepsDiagonalFlightAroundCornerInsteadOfBypassingBody)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 100, .y = 100, .z = 800 });
    world.spawnPlayer(2U, '#', { .x = 150, .y = 150, .z = 800 });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.player(1U).has_value());
    shared::Player const before = requiredValue(world.player(1U));
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .y = 127U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    double const before_x = shared::playerPositionX(before);
    double const before_y = shared::playerPositionY(before);
    double const after_x = shared::playerPositionX(requiredValue(world.player(1U)));
    double const after_y = shared::playerPositionY(requiredValue(world.player(1U)));
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
        .x = 127U, .y = 0U,
        .z = static_cast<uint8_t>(-127),
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_LT(requiredValue(world.player(1U)).x, 10);
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
    shared::Player const before = requiredValue(world.player(1U));
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U,
        .y = 127U,
        .accelerated = true,
        .speedup = 200U,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    double const before_x = shared::playerPositionX(before);
    double const before_y = shared::playerPositionY(before);
    double const after_x = shared::playerPositionX(requiredValue(world.player(1U)));
    double const after_y = shared::playerPositionY(requiredValue(world.player(1U)));
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

    EXPECT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(requiredValue(world.player(1U))), 805.0);

    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionX(requiredValue(world.player(1U))), 32'767.5);
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

    ASSERT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    double const first_height = shared::playerPositionZ(requiredValue(world.player(1U)));
    EXPECT_DOUBLE_EQ(first_height, static_cast<double>(terrain_height) + FIRST_TICK_HEIGHT_DELTA);
    EXPECT_EQ(requiredValue(world.player(1U)).vertical_velocity_subcells, FIRST_TICK_VERTICAL_VELOCITY);

    double maximum_height = first_height;
    for (uint8_t tick = 0U; tick < TICKS_TO_APEX; ++tick) {
        ASSERT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U }));
        ASSERT_TRUE(world.player(1U).has_value());
        maximum_height = std::max(maximum_height, shared::playerPositionZ(requiredValue(world.player(1U))));
    }
    EXPECT_NEAR(maximum_height, static_cast<double>(terrain_height) + MAXIMUM_HEIGHT_DELTA, 1e-9);
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), static_cast<double>(terrain_height));
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

    EXPECT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionY(requiredValue(world.player(1U))), 32'768.5);
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

    EXPECT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionX(requiredValue(world.player(1U))), 35'050.5);
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
    EXPECT_EQ(requiredValue(world.player(1)).x, 101);
    EXPECT_EQ(requiredValue(world.player(1)).y, 101);
    EXPECT_EQ(requiredValue(world.player(1)).z, 101);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 6'160U);
    EXPECT_EQ(requiredValue(world.player(1)).y_subcell, 6'160U);
    EXPECT_EQ(requiredValue(world.player(1)).z_subcell, 6'160U);
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
    EXPECT_EQ(requiredValue(world.player(1)).x, 125);
    EXPECT_EQ(requiredValue(world.player(1)).y, 125);
    EXPECT_EQ(requiredValue(world.player(1)).z, 125);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 8'560U);
    EXPECT_EQ(requiredValue(world.player(1)).y_subcell, 8'560U);
    EXPECT_EQ(requiredValue(world.player(1)).z_subcell, 8'560U);
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
    EXPECT_EQ(requiredValue(world.player(1U)).movement_capabilities.bits, 3U);
}

TEST(FlightWorldTest, RejectsRemovingCollisionBypassWhilePlayerIsEmbeddedInTerrain)
{
    static constexpr uint32_t X = 4U;
    static constexpr uint32_t Y = 4U;
    shared::TerrainGenerator const terrain;
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = X,
        .y = Y,
        .z = static_cast<uint16_t>(terrain.heightAt(X, Y) - 1U),
    });

    EXPECT_FALSE(world.canSetPlayerMovementCapabilities(1U, {}));
    EXPECT_FALSE(world.setPlayerMovementCapabilities(1U, {}));
    EXPECT_TRUE(world.canSetPlayerMovementCapabilities(1U, { .bits = 3U }));
}

TEST(FlightWorldTest, RejectsRemovingCollisionBypassInsideMaterializedStone)
{
    shared::SparseWorld blocks;
    ASSERT_TRUE(blocks.publishMaterializedChunk(
        shared::Chunk{{.x = 0, .y = 0, .z = 0}},
        blocks.options().revision,
        blocks.options().seed
    ));
    ASSERT_TRUE(blocks.setBlock({.x = 4, .y = 4, .z = 10}, shared::Block::Stone));

    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 4, .y = 4, .z = 10});
    world.setCollisionWorld(&blocks);

    EXPECT_FALSE(world.canSetPlayerMovementCapabilities(1U, {.bits = 1U}));
    EXPECT_FALSE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
    EXPECT_TRUE(world.canSetPlayerMovementCapabilities(1U, {.bits = 3U}));
}

TEST(FlightWorldTest, FlightIsGrantedPerPlayerRatherThanByClientIntent)
{
    shared::TerrainGenerator const terrain;
    uint16_t const terrain_height = terrain.heightAt(4, 4);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = terrain_height });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    EXPECT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(requiredValue(world.player(1U))), terrain_height);

    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    EXPECT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_GT(shared::playerPositionZ(requiredValue(world.player(1U))), terrain_height);
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
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::FLIGHT_MAX_CELL);
    EXPECT_EQ(requiredValue(world.player(1)).x_subcell, 4'400U);

    EXPECT_FALSE(world.setPlayerPosition(1, {
        .x = shared::World::FLIGHT_MIN_CELL,
        .y = 0,
        .z = shared::World::FLIGHT_MAX_Z + 1,
    }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::FLIGHT_MAX_CELL);
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
