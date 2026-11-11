#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>
#include <testsupport/TerrainFixtures.hpp>

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

bool publishMovementTestSurface(shared::SparseWorld& world, bool const one_block_step)
{
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights = testsupport::makeHeightSamples(386U);
    if (one_block_step) {
        for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint32_t x = 6U; x < shared::Chunk::SIDE_LENGTH; ++x) {
                heights[y * shared::Chunk::SIDE_LENGTH + x] = 387U;
            }
        }
    }
    auto const identity = testsupport::makeTerrainIdentity(world.options().seed, world.options().revision);
    if (!world.setTerrainIdentity(identity)
        || !world.publishAuthoritativeSurface(testsupport::makeSurfaceTile({ .x = 0, .y = 0 }, identity, heights))) {
        return false;
    }

    shared::Chunk::Blocks blocks;
    for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
            auto const column = world.authoritativeColumn(x, y);
            if (!column) {
                return false;
            }
            for (uint32_t local_z = 0U; local_z < shared::Chunk::SIDE_LENGTH; ++local_z) {
                uint32_t const z = 24U * shared::Chunk::SIDE_LENGTH + local_z;
                blocks[local_z * shared::Chunk::FACE_BLOCK_COUNT + y * shared::Chunk::SIDE_LENGTH + x]
                    = column->materialAt(static_cast<uint16_t>(z));
            }
        }
    }
    return world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 24 }, blocks },
        world.options().revision,
        world.options().seed
    );
}

bool publishDiagonalStepWall(shared::SparseWorld& world)
{
    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
            uint32_t const floor_index = shared::Chunk::FACE_BLOCK_COUNT
                + y * shared::Chunk::SIDE_LENGTH + x;
            blocks[floor_index] = shared::Block::Stone;
            if (x >= 6U && y < 6U) {
                blocks[2U * shared::Chunk::FACE_BLOCK_COUNT + y * shared::Chunk::SIDE_LENGTH + x]
                    = shared::Block::Stone;
            } else if (x < 6U && y >= 6U) {
                blocks[2U * shared::Chunk::FACE_BLOCK_COUNT + y * shared::Chunk::SIDE_LENGTH + x]
                    = shared::Block::Stone;
                blocks[3U * shared::Chunk::FACE_BLOCK_COUNT + y * shared::Chunk::SIDE_LENGTH + x]
                    = shared::Block::Stone;
            }
        }
    }
    return world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 24 }, blocks },
        world.options().revision,
        world.options().seed
    );
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

TEST(FlightWorldTest, ProfileOnlyTerrainSupportsGravityAndJumpingWithoutDenseChunks)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = testsupport::makeSurfaceTile({ .x = 0, .y = 0 }, identity,
        testsupport::makeHeightSamples(61U));
    shared::SparseWorld collision_world;
    ASSERT_TRUE(collision_world.setTerrainIdentity(identity));
    ASSERT_TRUE(collision_world.publishAuthoritativeSurface(surface));
    ASSERT_EQ(collision_world.residentChunkCount(), 0U);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 61 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    for (uint32_t tick = 0U; tick < 32U; ++tick) {
        static_cast<void>(world.movePlayer(1U, {}));
    }
    ASSERT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 61.0);
    ASSERT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 61.6125);
    for (uint32_t tick = 0U; tick < 32U; ++tick) {
        static_cast<void>(world.movePlayer(1U, {}));
    }
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 61.0);
    EXPECT_EQ(collision_world.residentChunkCount(), 0U);
}

TEST(FlightWorldTest, ProfileSupportPreservesEditedCavityFloorBelowThePlayer)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = testsupport::makeSurfaceTile({ .x = 0, .y = 0 }, identity,
        testsupport::makeHeightSamples(61U));
    shared::SparseWorld collision_world;
    ASSERT_TRUE(collision_world.setTerrainIdentity(identity));
    ASSERT_TRUE(collision_world.publishAuthoritativeSurface(surface));
    for (int32_t z = 10; z <= 25; ++z) {
        ASSERT_TRUE(collision_world.setBlock({ .x = 4, .y = 4, .z = z }, shared::Block::Air));
    }
    auto const edited_column = collision_world.authoritativeColumn(4, 4);
    ASSERT_TRUE(edited_column.has_value());
    if (!edited_column) {
        return;
    }
    ASSERT_NE(edited_column->dense_blocks, nullptr);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 12 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));

    for (uint32_t tick = 0U; tick < 32U; ++tick) {
        static_cast<void>(world.movePlayer(1U, {}));
    }
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 10.0);
}

TEST(FlightWorldTest, ProfileSupportBoundsTerrainQueriesWhileEntityHeightExceedsTerrain)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = testsupport::makeSurfaceTile({ .x = 0, .y = 0 }, identity,
        testsupport::makeHeightSamples(1'021U));
    shared::SparseWorld collision_world;
    ASSERT_TRUE(collision_world.setTerrainIdentity(identity));
    ASSERT_TRUE(collision_world.publishAuthoritativeSurface(surface));
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 65'000 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));
    ASSERT_TRUE(world.movePlayer(1U, {}));
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 64'999.9125);
    ASSERT_TRUE(world.setPlayerPosition(1U, { .x = 4, .y = 4, .z = 1'025 }));
    for (uint32_t tick = 0U; tick < 32U; ++tick) {
        static_cast<void>(world.movePlayer(1U, {}));
    }
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 1'021.0);
    EXPECT_EQ(collision_world.residentChunkCount(), 0U);
}

TEST(FlightWorldTest, ProfileSlopeAllowsDiagonalReverseAndRejectsEmbeddedBypassRemoval)
{
    auto heights = testsupport::makeHeightSamples(10U);
    for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
        for (uint32_t x = 6U; x < shared::Chunk::SIDE_LENGTH; ++x) {
            heights[y * shared::Chunk::SIDE_LENGTH + x] = 11U;
        }
    }
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = testsupport::makeSurfaceTile({ .x = 0, .y = 0 }, identity, heights);
    shared::SparseWorld collision_world;
    ASSERT_TRUE(collision_world.setTerrainIdentity(identity));
    ASSERT_TRUE(collision_world.publishAuthoritativeSurface(surface));
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 10 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U, .y = 0U, .z = 127U, .accelerated = true, .speedup = 15U,
    }));
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = static_cast<uint8_t>(-127), .y = 0U, .z = static_cast<uint8_t>(-127),
        .accelerated = true, .speedup = 15U,
    }));
    EXPECT_DOUBLE_EQ(shared::playerPositionX(requiredValue(world.player(1U))), 4.0);
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 10.0);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 3U }));
    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 0U, .y = 0U, .z = static_cast<uint8_t>(-127),
    }));
    EXPECT_FALSE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    ASSERT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    EXPECT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    EXPECT_EQ(collision_world.residentChunkCount(), 0U);
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

TEST(FlightWorldTest, EveryValidNonAirMaterialSupportsLanding)
{
    static constexpr std::array<shared::Block, 6> SOLID_MATERIALS{
        shared::Block::Stone,
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
    };
    static constexpr uint8_t TERRAIN_HEIGHT = 8U;
    static constexpr uint8_t FALL_TICKS = 32U;

    for (shared::Block const material : SOLID_MATERIALS) {
        shared::SparseWorld collision_world;
        shared::Chunk::Blocks blocks;
        blocks.fill(shared::Block::Air);
        for (uint8_t block_z = 0U; block_z < TERRAIN_HEIGHT; ++block_z) {
            uint32_t const block_index = static_cast<uint32_t>(block_z)
                    * shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH
                + 4U * shared::Chunk::SIDE_LENGTH + 4U;
            blocks[block_index] = material;
        }
        ASSERT_TRUE(collision_world.publishMaterializedChunk(
            shared::Chunk{ { .x = 0, .y = 0, .z = 0 }, blocks },
            collision_world.options().revision,
            collision_world.options().seed
        )) << "material: " << static_cast<uint32_t>(material);

        shared::World world{ shared::WorldMode::Flight };
        world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 16 });
        ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {}));
        world.setCollisionWorld(&collision_world);

        for (uint8_t tick = 0U; tick < FALL_TICKS; ++tick) {
            static_cast<void>(world.movePlayer(1U, { .x = 0U, .y = 0U }));
        }

        ASSERT_TRUE(world.player(1U).has_value());
        EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), TERRAIN_HEIGHT)
            << "material: " << static_cast<uint32_t>(material);
    }
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

TEST(FlightWorldTest, UpperBodyCollisionCandidatesLoadOnlyRequiredChunksBeforeMovement)
{
    for (int32_t const height : {1'023, 1'024, 2'047}) {
        SCOPED_TRACE(height);
        shared::SparseWorld blocks({.seed = 42U, .revision = 1U},
            shared::SparseWorldGenerationMode::MaterializedOnly);
        shared::World world{shared::WorldMode::Flight};
        world.spawnPlayer(1U, '@', {.x = 100, .y = 100, .z = height});
        ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
        world.setCollisionWorld(&blocks);
        shared::Direction const direction{.x = 127U, .y = 0U};
        EXPECT_FALSE(world.movePlayer(1U, direction));
        auto const chunks = world.flightCollisionChunks(1U, direction);
        int32_t const first_section = height / shared::Chunk::SIDE_LENGTH;
        int32_t const last_section = std::min<int32_t>(height + 1, shared::WorldExtent::DEPTH - 1U)
            / shared::Chunk::SIDE_LENGTH;
        for (int32_t section = first_section; section <= last_section; ++section) {
            EXPECT_TRUE(std::ranges::any_of(chunks, [section](shared::ChunkCoordinate const coordinate) {
                return coordinate.x == 6 && coordinate.y == 6 && coordinate.z == section;
            }));
        }
        for (auto const coordinate : chunks) {
            EXPECT_LT(coordinate.z, shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH);
            ASSERT_TRUE(blocks.publishMaterializedChunk(shared::Chunk{coordinate}, 1U, 42U));
        }
        EXPECT_TRUE(world.movePlayer(1U, direction));
    }
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

TEST(FlightWorldTest, MaterializedStepAllowsClearAscendingDiagonalAndReverseFlight)
{
    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
            uint32_t const ground_end = x >= 6U ? 11U : 10U;
            for (uint32_t z = 0U; z < ground_end; ++z) {
                blocks[z * shared::Chunk::FACE_BLOCK_COUNT + y * shared::Chunk::SIDE_LENGTH + x]
                    = shared::Block::Stone;
            }
        }
    }
    shared::SparseWorld collision_world;
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 0 }, blocks },
        collision_world.options().revision,
        collision_world.options().seed
    ));
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 1 } },
        collision_world.options().revision,
        collision_world.options().seed
    ));
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 10 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 127U, .y = 0U, .z = 127U, .accelerated = true, .speedup = 15U,
    }));
    shared::Player const elevated = requiredValue(world.player(1U));
    EXPECT_DOUBLE_EQ(shared::playerPositionX(elevated), 9.9265);
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(elevated), 15.9265);

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = static_cast<uint8_t>(-127), .y = 0U, .z = static_cast<uint8_t>(-127),
        .accelerated = true, .speedup = 15U,
    }));
    shared::Player const returned = requiredValue(world.player(1U));
    EXPECT_DOUBLE_EQ(shared::playerPositionX(returned), 4.0);
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(returned), 10.0);
}

TEST(FlightWorldTest, MaterializedWallStillBlocksTheSweptAscendingDiagonal)
{
    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    for (uint32_t z = 0U; z < shared::Chunk::SIDE_LENGTH; ++z) {
        for (uint32_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            blocks[z * shared::Chunk::FACE_BLOCK_COUNT + y * shared::Chunk::SIDE_LENGTH + 6U]
                = shared::Block::Stone;
        }
    }
    shared::SparseWorld collision_world;
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 0 }, blocks },
        collision_world.options().revision,
        collision_world.options().seed
    ));
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 1 } },
        collision_world.options().revision,
        collision_world.options().seed
    ));
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 10 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    static_cast<void>(world.movePlayer(1U, {
        .x = 127U, .y = 0U, .z = 127U, .accelerated = true, .speedup = 15U,
    }));
    shared::Player const stopped = requiredValue(world.player(1U));
    EXPECT_LE(shared::playerPositionX(stopped) + 0.625, 6.0);
    EXPECT_GT(shared::playerPositionZ(stopped), 10.0);
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

TEST(FlightWorldTest, CreativeFlightWithCollisionKeepsMovingAcrossFlatAuthoritativeSurface)
{
    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishMovementTestSurface(collision_world, false));
    auto const support = collision_world.authoritativeColumn(4, 4);
    ASSERT_TRUE(support.has_value());
    EXPECT_EQ(support->highest_solid_z, 385U);
    EXPECT_EQ(collision_world.queryBlock({ .x = 4, .y = 4, .z = 386 }).block, shared::Block::Air);
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 386 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    shared::Player const before = requiredValue(world.player(1U));

    for (uint32_t tick = 0U; tick < 10U; ++tick) {
        ASSERT_TRUE(world.movePlayer(1U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK))
            << "held horizontal input stopped on flat materialized ground at tick " << tick;
    }

    shared::Player const after = requiredValue(world.player(1U));
    EXPECT_EQ(after.movement_capabilities.bits, 1U);
    EXPECT_GT(shared::playerPositionX(after), shared::playerPositionX(before));
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(after), 386.0);
}

TEST(FlightWorldTest, CreativeFlightKeepsHorizontalMotionWhileDescendingIntoFlatGround)
{
    static constexpr uint32_t HORIZONTAL_TICKS = 10U;
    static constexpr uint32_t HORIZONTAL_DELTA_SUBCELLS_PER_TICK =
        shared::MOVEMENT_SUBCELLS_PER_TICK * 127U / 180U;
    static constexpr double EXPECTED_HORIZONTAL_DELTA = static_cast<double>(
        HORIZONTAL_TICKS * HORIZONTAL_DELTA_SUBCELLS_PER_TICK
    ) / shared::SUBCELLS_PER_CELL;

    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishMovementTestSurface(collision_world, false));
    auto const support = collision_world.authoritativeColumn(4U, 4U);
    ASSERT_TRUE(support.has_value());
    EXPECT_EQ(support->highest_solid_z, 385U);
    EXPECT_EQ(collision_world.queryBlock({ .x = 4, .y = 4, .z = 386 }).block, shared::Block::Air);

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 386 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    shared::Player const before = requiredValue(world.player(1U));

    for (uint32_t tick = 0U; tick < HORIZONTAL_TICKS; ++tick) {
        ASSERT_TRUE(world.movePlayer(1U, {
            .x = 127U,
            .y = 0U,
            .z = static_cast<uint8_t>(-127),
            .accelerated = false,
            .speedup = 5U,
        }, shared::TICK)) << "horizontal input stopped while Shift-style descent met flat ground at tick " << tick;
        EXPECT_DOUBLE_EQ(shared::playerPositionZ(requiredValue(world.player(1U))), 386.0);
    }

    shared::Player const after = requiredValue(world.player(1U));
    EXPECT_NEAR(
        shared::playerPositionX(after) - shared::playerPositionX(before),
        EXPECTED_HORIZONTAL_DELTA,
        1e-9
    );
}

TEST(FlightWorldTest, DescendingFlatGroundRetainsBothHorizontalComponents)
{
    for (auto const heading : std::array{std::array{127, 127}, std::array{127, -63},
             std::array{-63, 127}, std::array{-127, -127}}) {
        for (uint16_t const subcell : {uint16_t{0}, uint16_t{7'500}}) {
            SCOPED_TRACE(::testing::Message() << heading[0] << "," << heading[1] << ":" << subcell);
            shared::SparseWorld collision_world;
            ASSERT_TRUE(publishMovementTestSurface(collision_world, false));
            shared::World world{shared::WorldMode::Flight};
            world.spawnPlayer(1U, '@', {.x = 8, .y = 8, .z = 386,
                .x_subcell = subcell, .y_subcell = subcell});
            world.setCollisionWorld(&collision_world);
            ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
            auto const before = requiredValue(world.player(1U));
            for (uint32_t tick = 0U; tick < 5U; ++tick) {
                ASSERT_TRUE(world.movePlayer(1U, {.x = static_cast<uint8_t>(heading[0]),
                    .y = static_cast<uint8_t>(heading[1]), .z = static_cast<uint8_t>(-127)}));
                auto const after = requiredValue(world.player(1U));
                EXPECT_DOUBLE_EQ(shared::playerPositionZ(after), 386.0);
                EXPECT_GT((shared::playerPositionX(after) - shared::playerPositionX(before)) * heading[0], 0.0);
                EXPECT_GT((shared::playerPositionY(after) - shared::playerPositionY(before)) * heading[1], 0.0);
            }
        }
    }
}

TEST(FlightWorldTest, CancelledVerticalInputKeepsDiagonalGroundMovement)
{
    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishMovementTestSurface(collision_world, false));
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 4, .y = 4, .z = 386});
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
    // Space and Shift cancel before transmission, producing zero vertical input.
    ASSERT_TRUE(world.movePlayer(1U, {.x = 127U, .y = 127U, .z = 0U}));
    auto const after = requiredValue(world.player(1U));
    EXPECT_GT(shared::playerPositionX(after), 4.0);
    EXPECT_GT(shared::playerPositionY(after), 4.0);
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(after), 386.0);
}

TEST(FlightWorldTest, DescendingGroundSlideRetainsWallCollision)
{
    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishMovementTestSurface(collision_world, false));
    for (int64_t y = 0; y < 16; ++y) {
        ASSERT_TRUE(collision_world.setBlock({.x = 6, .y = y, .z = 386}, shared::Block::Stone));
        ASSERT_TRUE(collision_world.setBlock({.x = 6, .y = y, .z = 387}, shared::Block::Stone));
    }
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 4, .y = 4, .z = 386});
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
    ASSERT_TRUE(world.movePlayer(1U, {.x = 127U, .y = 127U,
        .z = static_cast<uint8_t>(-127), .accelerated = true, .speedup = 5U}));
    auto const after = requiredValue(world.player(1U));
    EXPECT_LT(shared::playerPositionX(after), 6.0);
    EXPECT_GT(shared::playerPositionY(after), 4.0);
    EXPECT_DOUBLE_EQ(shared::playerPositionZ(after), 386.0);
}

TEST(FlightWorldTest, DescendingGroundSlideDoesNotBypassDiagonalPlayer)
{
    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishMovementTestSurface(collision_world, false));
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', {.x = 4, .y = 4, .z = 386});
    world.spawnPlayer(2U, '#', {.x = 5, .y = 5, .z = 386});
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, {.bits = 1U}));
    ASSERT_TRUE(world.movePlayer(1U, {.x = 127U, .y = 127U,
        .z = static_cast<uint8_t>(-127), .accelerated = true, .speedup = 5U}));
    auto const after = requiredValue(world.player(1U));
    EXPECT_TRUE(shared::playerPositionX(after) == 4.0 || shared::playerPositionY(after) == 4.0);
}

TEST(FlightWorldTest, OneBlockStepNeedsFlightLiftOrGroundedJump)
{
    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishMovementTestSurface(collision_world, true));
    auto const lower_support = collision_world.authoritativeColumn(4, 4);
    auto const raised_support = collision_world.authoritativeColumn(6, 4);
    ASSERT_TRUE(lower_support.has_value());
    ASSERT_TRUE(raised_support.has_value());
    EXPECT_EQ(lower_support->highest_solid_z, 385U);
    EXPECT_EQ(raised_support->highest_solid_z, 386U);
    EXPECT_EQ(collision_world.queryBlock({ .x = 6, .y = 4, .z = 386 }).block, shared::Block::Grass);

    shared::World flight_world{ shared::WorldMode::Flight };
    flight_world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 386 });
    flight_world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(flight_world.setPlayerMovementCapabilities(1U, { .bits = 1U }));
    for (uint32_t tick = 0U; tick < 12U; ++tick) {
        if (!flight_world.movePlayer(1U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK)) {
            break;
        }
    }
    shared::Player const flight_blocked = requiredValue(flight_world.player(1U));
    double const flight_blocked_x = shared::playerPositionX(flight_blocked);
    EXPECT_GT(flight_blocked_x, 4.0);
    EXPECT_LT(flight_blocked_x, 6.0);
    EXPECT_FALSE(flight_world.movePlayer(1U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK));
    ASSERT_TRUE(flight_world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }, shared::TICK));
    ASSERT_TRUE(flight_world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }, shared::TICK));
    ASSERT_TRUE(flight_world.movePlayer(1U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK));
    EXPECT_GT(shared::playerPositionX(requiredValue(flight_world.player(1U))), flight_blocked_x);

    shared::World grounded_world{ shared::WorldMode::Flight };
    grounded_world.spawnPlayer(2U, '#', { .x = 4, .y = 4, .z = 386 });
    grounded_world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(grounded_world.setPlayerMovementCapabilities(2U, {}));
    for (uint32_t tick = 0U; tick < 12U; ++tick) {
        if (!grounded_world.movePlayer(2U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK)) {
            break;
        }
    }
    shared::Player const grounded_blocked = requiredValue(grounded_world.player(2U));
    double const grounded_blocked_x = shared::playerPositionX(grounded_blocked);
    EXPECT_GT(grounded_blocked_x, 4.0);
    EXPECT_LT(grounded_blocked_x, 6.0);
    EXPECT_FALSE(grounded_world.movePlayer(2U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK));
    ASSERT_TRUE(grounded_world.movePlayer(2U, { .x = 0U, .y = 0U, .z = 127U }, shared::TICK));
    ASSERT_TRUE(grounded_world.movePlayer(2U, {}, shared::TICK));
    ASSERT_TRUE(grounded_world.movePlayer(2U, { .x = 127U, .y = 0U, .z = 0U }, shared::TICK));
    EXPECT_GT(shared::playerPositionX(requiredValue(grounded_world.player(2U))), grounded_blocked_x);
}

TEST(FlightWorldTest, DiagonalFlightKeepsTheClearAxisCandidateHeightAcrossAStep)
{
    shared::SparseWorld collision_world;
    ASSERT_TRUE(publishDiagonalStepWall(collision_world));
    EXPECT_EQ(collision_world.queryBlock({ .x = 6, .y = 4, .z = 386 }).block, shared::Block::Stone);
    EXPECT_EQ(collision_world.queryBlock({ .x = 5, .y = 6, .z = 387 }).block, shared::Block::Stone);

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 5,
        .y = 4,
        .z = 386,
        .y_subcell = 9'000U,
    });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 35U,
        .y = 127U,
        .z = 127U,
        .accelerated = true,
        .speedup = 5U,
    }));

    shared::Player const moved = requiredValue(world.player(1U));
    EXPECT_GT(shared::playerPositionX(moved), 5.375);
    EXPECT_DOUBLE_EQ(shared::playerPositionY(moved), 4.9);
    EXPECT_GE(shared::playerPositionZ(moved), 387.0);
}

TEST(FlightWorldTest, DiagonalFallbackSamplesVerticalTravelBeforePassingACeiling)
{
    static constexpr std::array<int32_t, 2U> EMPTY_CHUNK_ZS{ 7, 8 };
    static constexpr double CEILING_ENTRY_X = 6.0
        - static_cast<double>(shared::World::PLAYER_WIDTH_SUBCELLS) / shared::SUBCELLS_PER_CELL;

    shared::SparseWorld collision_world;
    shared::Chunk::Blocks lower_chunk;
    lower_chunk.fill(shared::Block::Air);
    lower_chunk[6U * shared::Chunk::FACE_BLOCK_COUNT + 6U * shared::Chunk::SIDE_LENGTH + 4U]
        = shared::Block::Stone;
    lower_chunk[9U * shared::Chunk::FACE_BLOCK_COUNT + 4U * shared::Chunk::SIDE_LENGTH + 6U]
        = shared::Block::Stone;
    ASSERT_TRUE(collision_world.publishMaterializedChunk(
        shared::Chunk{ { .x = 0, .y = 0, .z = 6 }, lower_chunk },
        collision_world.options().revision,
        collision_world.options().seed
    ));
    shared::Chunk::Blocks empty_chunk;
    empty_chunk.fill(shared::Block::Air);
    for (int32_t const chunk_z : EMPTY_CHUNK_ZS) {
        ASSERT_TRUE(collision_world.publishMaterializedChunk(
            shared::Chunk{ { .x = 0, .y = 0, .z = chunk_z }, empty_chunk },
            collision_world.options().revision,
            collision_world.options().seed
        ));
    }
    EXPECT_EQ(collision_world.queryBlock({ .x = 4, .y = 6, .z = 102 }).block, shared::Block::Stone);
    EXPECT_EQ(collision_world.queryBlock({ .x = 6, .y = 4, .z = 105 }).block, shared::Block::Stone);

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = 4, .y = 4, .z = 100 });
    world.setCollisionWorld(&collision_world);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 1U }));

    ASSERT_TRUE(world.movePlayer(1U, {
        .x = 30U,
        .y = 127U,
        .z = 127U,
        .accelerated = true,
        .speedup = 80U,
    }));

    shared::Player const moved = requiredValue(world.player(1U));
    EXPECT_GT(shared::playerPositionX(moved), 4.25);
    EXPECT_LT(shared::playerPositionX(moved), CEILING_ENTRY_X);
    EXPECT_GT(shared::playerPositionZ(moved), 110.0);
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
        .z = 65'536,
    }));
    ASSERT_TRUE(world.player(1).has_value());
    EXPECT_EQ(requiredValue(world.player(1)).x, shared::World::FLIGHT_MAX_CELL);
}

TEST(FlightWorldTest, EntityHeightCrossesTerrainCeilingAndStopsAtEntityLimit)
{
    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', {
        .x = 32'768,
        .y = 32'768,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH - 1U),
    });
    ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 3U }));
    ASSERT_TRUE(world.setPlayerPosition(1U, {
        .x = 32'768,
        .y = 32'768,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH - 1U),
        .z_subcell = 9'990U,
    }));
    ASSERT_TRUE(world.movePlayer(1U, { .x = 0U, .y = 0U, .z = 127U }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(requiredValue(world.player(1U)).z, static_cast<int32_t>(shared::WorldExtent::DEPTH));

    EXPECT_TRUE(world.setPlayerPosition(1U, {
        .x = 32'768,
        .y = 32'768,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH),
    }));
    EXPECT_TRUE(world.setPlayerPosition(1U, {
        .x = 32'768,
        .y = 32'768,
        .z = 65'535,
        .z_subcell = shared::SUBCELLS_PER_CELL - 1U,
    }));
    EXPECT_FALSE(world.setPlayerPosition(1U, {
        .x = 32'768,
        .y = 32'768,
        .z = 65'536,
    }));
    ASSERT_TRUE(world.player(1U).has_value());
    EXPECT_EQ(requiredValue(world.player(1U)).z, 65'535);
    EXPECT_EQ(requiredValue(world.player(1U)).z_subcell, shared::SUBCELLS_PER_CELL - 1U);
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
