#include <shared/world/SparseWorld.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

TEST(SparseWorldTest, ExposesTheLogicalBoundsAndPositiveHorizontalWrapping)
{
    static constexpr int64_t WIDTH = shared::WorldExtent::WIDTH;
    static constexpr shared::WorldCoordinate NEGATIVE{-1, -WIDTH - 2, 0};
    static constexpr shared::WorldCoordinate EXPECTED{WIDTH - 1, WIDTH - 2, 0};

    auto const normalized = shared::WorldBounds::normalize(NEGATIVE);

    ASSERT_TRUE(normalized.has_value());
    EXPECT_EQ(*normalized, EXPECTED);
    EXPECT_EQ(shared::WorldBounds::wrapHorizontal(WIDTH), 0U);
    EXPECT_EQ(shared::WorldBounds::wrapHorizontal(-WIDTH), 0U);
    EXPECT_FALSE(shared::WorldBounds::normalize({0, 0, -1}).has_value());
    EXPECT_FALSE(shared::WorldBounds::normalize({0, 0, WIDTH}).has_value());
}

TEST(SparseWorldTest, LazilyMaterializesAndBoundsResidentChunks)
{
    static constexpr uint64_t MAX_RESIDENT = 2U;
    static constexpr shared::WorldCoordinate FIRST{0, 0, 0};
    static constexpr shared::WorldCoordinate SECOND{16, 0, 0};
    static constexpr shared::WorldCoordinate THIRD{32, 0, 0};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});

    EXPECT_EQ(world.residentChunkCount(), 0U);
    ASSERT_TRUE(world.blockAt(FIRST).has_value());
    ASSERT_TRUE(world.blockAt(SECOND).has_value());
    EXPECT_EQ(world.residentChunkCount(), MAX_RESIDENT);
    ASSERT_TRUE(world.blockAt(THIRD).has_value());
    EXPECT_EQ(world.residentChunkCount(), MAX_RESIDENT);
    EXPECT_EQ(world.residentChunk({.x = 0, .y = 0, .z = 0}), nullptr);
}

TEST(SparseWorldTest, WrappedCoordinatesResolveToTheSameResidentChunk)
{
    static constexpr int64_t WIDTH = shared::WorldExtent::WIDTH;
    static constexpr shared::WorldCoordinate ORIGINAL{7, 11, 0};
    static constexpr shared::WorldCoordinate WRAPPED{7 - WIDTH, 11 + WIDTH, 0};

    shared::SparseWorld world;
    ASSERT_TRUE(world.blockAt(ORIGINAL).has_value());
    ASSERT_TRUE(world.blockAt(WRAPPED).has_value());
    EXPECT_EQ(world.residentChunkCount(), 1U);
    EXPECT_EQ(world.blockAt(ORIGINAL), world.blockAt(WRAPPED));
}

TEST(SparseWorldTest, NonBlockingQuerySeparatesUnknownPreviewAndMaterializedData)
{
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 2, .y = 3, .z = 4};
    static constexpr shared::WorldCoordinate BLOCK_COORDINATE{32, 48, 64};
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;

    shared::SparseWorld world({.seed = WORLD_SEED, .max_resident_chunks = 2U});
    shared::SparseBlockQuery const unknown = world.queryBlock(BLOCK_COORDINATE);
    EXPECT_EQ(unknown.state, shared::GenerationState::Unknown);
    EXPECT_FALSE(unknown.block.has_value());
    EXPECT_EQ(world.residentChunkCount(), 0U);

    shared::HeightTile preview{.coordinate = {.x = COORDINATE.x, .y = COORDINATE.y}};
    preview.heights.fill(12U);
    ASSERT_TRUE(world.publishPreview(COORDINATE, std::move(preview), WORLD_REVISION, WORLD_SEED));
    shared::SparseBlockQuery const previewed = world.queryBlock(BLOCK_COORDINATE);
    EXPECT_EQ(previewed.state, shared::GenerationState::Preview);
    EXPECT_FALSE(previewed.block.has_value());
    EXPECT_EQ(world.residentChunkCount(), 0U);

    shared::Chunk materialized{COORDINATE};
    ASSERT_TRUE(materialized.setBlock({.x = 0U, .y = 0U, .z = 0U}, shared::Block::Stone));
    ASSERT_TRUE(world.publishMaterializedChunk(std::move(materialized), WORLD_REVISION, WORLD_SEED));
    shared::SparseBlockQuery const known = world.queryBlock(BLOCK_COORDINATE);
    EXPECT_EQ(known.state, shared::GenerationState::Materialized);
    EXPECT_EQ(known.block, shared::Block::Stone);
    EXPECT_EQ(world.previewCount(), 0U);
}

TEST(SparseWorldTest, RejectsStaleMaterializationAndClearsOldRevision)
{
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 1, .y = 1, .z = 1};
    static constexpr shared::WorldCoordinate BLOCK_COORDINATE{16, 16, 16};
    static constexpr uint64_t CURRENT_REVISION = 9U;
    static constexpr uint64_t CURRENT_SEED = 73U;

    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{COORDINATE}, 1U, 42U));
    world.setWorldIdentity(CURRENT_REVISION, CURRENT_SEED);
    EXPECT_EQ(world.residentChunkCount(), 0U);
    EXPECT_FALSE(world.publishMaterializedChunk(shared::Chunk{COORDINATE}, 1U, 42U));
    EXPECT_FALSE(world.publishMaterializedChunk(shared::Chunk{COORDINATE}, CURRENT_REVISION, 42U));
    EXPECT_EQ(world.queryBlock(BLOCK_COORDINATE).state, shared::GenerationState::Unknown);
    EXPECT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{COORDINATE},
        CURRENT_REVISION,
        CURRENT_SEED
    ));
    EXPECT_EQ(world.queryBlock(BLOCK_COORDINATE).state, shared::GenerationState::Materialized);
}

TEST(SparseWorldTest, PreviewAndMaterializedResidencyShareOneBound)
{
    shared::SparseWorld world({.max_resident_chunks = 1U});
    shared::HeightTile preview{.coordinate = {.x = 1, .y = 1}};
    ASSERT_TRUE(world.publishPreview({.x = 1, .y = 1, .z = 0}, std::move(preview), 1U, 42U));

    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{{.x = 2, .y = 2, .z = 0}}, 1U, 42U));

    EXPECT_EQ(world.previewCount(), 0U);
    EXPECT_EQ(world.residentChunkCount(), 1U);
    EXPECT_EQ(world.queryBlock({32, 32, 0}).state, shared::GenerationState::Materialized);
}

TEST(SparseWorldTest, DistantTravelEvictsAndAllowsDeterministicRegeneration)
{
    static constexpr uint64_t MAX_RESIDENT = 2U;
    static constexpr uint64_t WORLD_REVISION = 3U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate ORIGIN{.x = 1, .y = 1, .z = 0};
    static constexpr shared::WorldCoordinate ORIGIN_BLOCK{16, 16, 0};

    shared::SparseWorld world({
        .seed = WORLD_SEED,
        .max_resident_chunks = MAX_RESIDENT,
        .revision = WORLD_REVISION,
    });
    shared::TerrainGenerator generator;
    shared::Chunk const expected = generator.generateChunk(ORIGIN);
    shared::ChunkContentIdentity const identity = expected.contentIdentity();
    ASSERT_TRUE(world.publishMaterializedChunk(generator.generateChunk(ORIGIN), WORLD_REVISION, WORLD_SEED));

    for (int32_t offset = 1; offset <= 128; ++offset) {
        shared::ChunkCoordinate const distant{.x = ORIGIN.x + offset * 16, .y = ORIGIN.y, .z = 0};
        ASSERT_TRUE(world.publishMaterializedChunk(generator.generateChunk(distant), WORLD_REVISION, WORLD_SEED));
        EXPECT_LE(world.residentChunkCount(), MAX_RESIDENT);
    }
    EXPECT_EQ(world.queryBlock(ORIGIN_BLOCK).state, shared::GenerationState::Unknown);
    ASSERT_TRUE(world.publishMaterializedChunk(generator.generateChunk(ORIGIN), WORLD_REVISION, WORLD_SEED));
    ASSERT_NE(world.residentChunk(ORIGIN), nullptr);
    EXPECT_EQ(world.residentChunk(ORIGIN)->contentIdentity(), identity);
}
