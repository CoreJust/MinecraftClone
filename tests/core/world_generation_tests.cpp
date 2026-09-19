#include <shared/world/WorldGeneration.hpp>

#include <gtest/gtest.h>

#include <cstdint>

TEST(WorldGenerationTest, FormulaHasPublishedBaseCenterAndFirstCrest)
{
    static constexpr int64_t CENTER = 32'768;
    static constexpr int64_t FIRST_RING = 32'832;
    static constexpr uint16_t BASE_HEIGHT = 6U;
    static constexpr uint16_t CENTER_HEIGHT = 806U;
    static constexpr uint16_t FIRST_RING_HEIGHT = 406U;

    EXPECT_EQ(shared::TerrainGenerator::nativeHeightAt(CENTER, CENTER), CENTER_HEIGHT);
    EXPECT_EQ(shared::TerrainGenerator::nativeHeightAt(CENTER + FIRST_RING - CENTER, CENTER), FIRST_RING_HEIGHT);
    EXPECT_EQ(shared::TerrainGenerator::nativeHeightAt(CENTER + 32, CENTER), BASE_HEIGHT);
}

TEST(WorldGenerationTest, HeightTileIsDeterministicAndUsesCoreLangScript)
{
    static constexpr shared::HeightTileCoordinate TILE{.x = 2'048, .y = 2'048};
    static constexpr shared::BlockCoordinate SAMPLE{.x = 3, .y = 7, .z = 0};

    shared::TerrainGenerator generator;
    shared::HeightTile const first = generator.generateHeightTile(TILE);
    shared::HeightTile const repeated = generator.generateHeightTile(TILE);

    ASSERT_TRUE(generator.usingCoreLang());
    EXPECT_EQ(first.coordinate, TILE);
    EXPECT_EQ(first.heights, repeated.heights);
    ASSERT_TRUE(first.heightAt(SAMPLE).has_value());
    EXPECT_EQ(*first.heightAt(SAMPLE), shared::TerrainGenerator::nativeHeightAt(32'771, 32'775));
}

TEST(WorldGenerationTest, ChunksContainStoneBelowTheGeneratedHeight)
{
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 2'048, .y = 2'048, .z = 0};
    static constexpr shared::BlockCoordinate SAMPLE{.x = 3, .y = 7, .z = 0};

    shared::TerrainGenerator generator;
    shared::Chunk const chunk = generator.generateChunk(COORDINATE);

    EXPECT_EQ(chunk.blockAt(SAMPLE), shared::Block::Stone);
    EXPECT_EQ(chunk.blockAt({.x = SAMPLE.x, .y = SAMPLE.y, .z = 15U}), shared::Block::Stone);
}
