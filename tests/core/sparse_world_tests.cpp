#include <shared/world/SparseWorld.hpp>

#include <gtest/gtest.h>

#include <cstdint>

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
