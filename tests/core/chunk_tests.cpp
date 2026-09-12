#include <shared/world/Chunk.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>

TEST(ChunkTest, StoresDenseCubicBlocksAtEveryBoundary)
{
    static constexpr shared::BlockCoordinate FIRST{ .x = 0, .y = 0, .z = 0 };
    static constexpr shared::BlockCoordinate LAST{ .x = 15, .y = 15, .z = 15 };

    shared::Chunk chunk;
    EXPECT_EQ(sizeof(shared::Block), 1U);
    EXPECT_EQ(sizeof(shared::Chunk::Blocks), shared::Chunk::BLOCK_COUNT);
    EXPECT_EQ(shared::Chunk::BLOCK_COUNT, 4'096U);
    EXPECT_EQ(chunk.blockAt(FIRST), shared::Block::Air);
    ASSERT_TRUE(chunk.setBlock(LAST, shared::Block::Stone));
    EXPECT_EQ(chunk.blockAt(LAST), shared::Block::Stone);
}

TEST(ChunkTest, RejectsOutOfBoundsQueriesAndEditsWithoutChangingIdentity)
{
    static constexpr shared::BlockCoordinate INVALID_X{ .x = 16, .y = 0, .z = 0 };
    static constexpr shared::BlockCoordinate INVALID_Y{ .x = 0, .y = 16, .z = 0 };
    static constexpr shared::BlockCoordinate INVALID_Z{ .x = 0, .y = 0, .z = 16 };

    shared::Chunk chunk;
    shared::ChunkContentIdentity const original_identity = chunk.contentIdentity();

    EXPECT_FALSE(chunk.blockAt(INVALID_X).has_value());
    EXPECT_FALSE(chunk.blockAt(INVALID_Y).has_value());
    EXPECT_FALSE(chunk.blockAt(INVALID_Z).has_value());
    EXPECT_FALSE(chunk.setBlock(INVALID_X, shared::Block::Stone));
    EXPECT_FALSE(chunk.setBlock(INVALID_Y, shared::Block::Stone));
    EXPECT_FALSE(chunk.setBlock(INVALID_Z, shared::Block::Stone));
    EXPECT_EQ(chunk.contentIdentity(), original_identity);
}

TEST(ChunkTest, RevisionsAndContentHashesChangeOnlyForEffectiveEdits)
{
    static constexpr shared::BlockCoordinate COORDINATE{ .x = 4, .y = 8, .z = 12 };

    shared::Chunk chunk;
    shared::ChunkContentIdentity const initial = chunk.contentIdentity();

    ASSERT_TRUE(chunk.setBlock(COORDINATE, shared::Block::Stone));
    shared::ChunkContentIdentity const stone = chunk.contentIdentity();
    EXPECT_EQ(stone.revision, initial.revision + 1U);
    EXPECT_NE(stone.content_hash, initial.content_hash);
    EXPECT_FALSE(chunk.setBlock(COORDINATE, shared::Block::Stone));
    EXPECT_EQ(chunk.contentIdentity(), stone);

    ASSERT_TRUE(chunk.setBlock(COORDINATE, shared::Block::Air));
    EXPECT_EQ(chunk.contentIdentity().revision, stone.revision + 1U);
    EXPECT_EQ(chunk.contentIdentity().content_hash, initial.content_hash);
}

TEST(ChunkTest, RejectsUnsupportedBlockEditsWithoutChangingIdentity)
{
    static constexpr shared::BlockCoordinate COORDINATE{ .x = 4, .y = 8, .z = 12 };
    static constexpr shared::Block UNSUPPORTED = static_cast<shared::Block>(255U);

    shared::Chunk chunk;
    shared::ChunkContentIdentity const original_identity = chunk.contentIdentity();

    EXPECT_FALSE(chunk.setBlock(COORDINATE, UNSUPPORTED));
    EXPECT_EQ(chunk.blockAt(COORDINATE), shared::Block::Air);
    EXPECT_EQ(chunk.contentIdentity(), original_identity);
}

TEST(ChunkTest, BulkConstructionPreservesCoordinateAndStartsAtRevisionZero)
{
    static constexpr shared::ChunkCoordinate CHUNK_COORDINATE{ .x = -4, .y = 7, .z = 11 };
    static constexpr shared::BlockCoordinate BLOCK_COORDINATE{ .x = 5, .y = 6, .z = 7 };

    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    blocks[static_cast<uint32_t>(BLOCK_COORDINATE.z) * shared::Chunk::FACE_BLOCK_COUNT
        + static_cast<uint32_t>(BLOCK_COORDINATE.y) * shared::Chunk::SIDE_LENGTH
        + BLOCK_COORDINATE.x] = shared::Block::Stone;
    shared::Chunk const chunk{ CHUNK_COORDINATE, blocks };

    EXPECT_EQ(chunk.coordinate(), CHUNK_COORDINATE);
    EXPECT_EQ(chunk.blockAt(BLOCK_COORDINATE), shared::Block::Stone);
    EXPECT_EQ(chunk.contentIdentity().revision, 0U);
}

TEST(ChunkTest, BulkConstructionRejectsUnsupportedBlocks)
{
    static constexpr shared::BlockCoordinate BLOCK_COORDINATE{ .x = 5, .y = 6, .z = 7 };

    shared::Chunk::Blocks blocks;
    blocks.fill(shared::Block::Air);
    blocks[static_cast<uint32_t>(BLOCK_COORDINATE.z) * shared::Chunk::FACE_BLOCK_COUNT
        + static_cast<uint32_t>(BLOCK_COORDINATE.y) * shared::Chunk::SIDE_LENGTH
        + BLOCK_COORDINATE.x] = static_cast<shared::Block>(2U);

    EXPECT_THROW(
        static_cast<void>(shared::Chunk{ {}, blocks }),
        std::invalid_argument
    );
}

TEST(ChunkTest, FixtureSamplingIsRepeatableAndBoundedForSignedChunkCoordinates)
{
    static constexpr shared::ChunkCoordinate CHUNK_COORDINATE{ .x = -123, .y = 456, .z = -789 };
    static constexpr shared::BlockCoordinate BLOCK_COORDINATE{ .x = 15, .y = 7, .z = 3 };
    static constexpr uint64_t SEED = 99U;

    int8_t const first = shared::Chunk::sampleFixtureNoise(CHUNK_COORDINATE, BLOCK_COORDINATE, SEED);
    int8_t const second = shared::Chunk::sampleFixtureNoise(CHUNK_COORDINATE, BLOCK_COORDINATE, SEED);

    EXPECT_EQ(first, second);
    EXPECT_GE(first, -3);
    EXPECT_LE(first, 3);
}

TEST(ChunkTest, StoneFixtureUsesThePublishedFormulaWithDefaultSeed)
{
    static constexpr shared::ChunkCoordinate CHUNK_COORDINATE{ .x = 2, .y = -3, .z = 4 };
    static constexpr uint8_t LAST_SOLID_LAYER = 2;
    static constexpr uint8_t FIRST_AIR_LAYER = 9;
    static constexpr uint64_t DIFFERENT_SEED = 43U;

    shared::Chunk const default_fixture = shared::Chunk::makeStoneFixture(CHUNK_COORDINATE);
    shared::Chunk const explicit_fixture = shared::Chunk::makeStoneFixture(
        CHUNK_COORDINATE,
        shared::Chunk::DEFAULT_FIXTURE_SEED
    );
    EXPECT_EQ(default_fixture.contentIdentity(), explicit_fixture.contentIdentity());
    EXPECT_NE(
        default_fixture.contentIdentity(),
        shared::Chunk::makeStoneFixture(CHUNK_COORDINATE, DIFFERENT_SEED).contentIdentity()
    );

    for (uint8_t z = 0; z < shared::Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0; x < shared::Chunk::SIDE_LENGTH; ++x) {
                shared::BlockCoordinate const coordinate{ .x = x, .y = y, .z = z };
                int32_t const height = static_cast<int32_t>(z) - 6
                    + shared::Chunk::sampleFixtureNoise(CHUNK_COORDINATE, coordinate);
                shared::Block const expected = height < 0 ? shared::Block::Stone : shared::Block::Air;
                EXPECT_EQ(default_fixture.blockAt(coordinate), expected);
            }
        }
    }

    for (uint8_t z = 0; z < shared::Chunk::SIDE_LENGTH; ++z) {
        shared::Block const guaranteed_block = z <= LAST_SOLID_LAYER
            ? shared::Block::Stone
            : shared::Block::Air;
        if (z > LAST_SOLID_LAYER && z < FIRST_AIR_LAYER) {
            continue;
        }
        for (uint8_t y = 0; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0; x < shared::Chunk::SIDE_LENGTH; ++x) {
                EXPECT_EQ(
                    default_fixture.blockAt({ .x = x, .y = y, .z = z }),
                    guaranteed_block
                );
            }
        }
    }
}
