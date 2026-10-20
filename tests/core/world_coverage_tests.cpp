#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

TEST(WorldCoverageTest, ClosedSphereKeepsFaceTangenciesAndRejectsOutsideCorners)
{
    static constexpr shared::PlayerPosition CENTER{.x = 15, .y = 15, .z = 8};
    static constexpr uint32_t RADIUS_BLOCKS = 1U;
    static constexpr std::array<shared::ChunkCoordinate, 3U> EXPECTED{
        shared::ChunkCoordinate{.x = 0, .y = 0, .z = 0},
        shared::ChunkCoordinate{.x = 1, .y = 0, .z = 0},
        shared::ChunkCoordinate{.x = 0, .y = 1, .z = 0},
    };

    std::vector<shared::ChunkCoordinate> const keys = shared::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );

    ASSERT_EQ(keys.size(), EXPECTED.size());
    EXPECT_TRUE(std::equal(keys.begin(), keys.end(), EXPECTED.begin()));
}

TEST(WorldCoverageTest, ExactChunkCornerIncludesEveryClosedNeighborInStableOrder)
{
    static constexpr shared::PlayerPosition CENTER{.x = 16, .y = 16, .z = 16};
    static constexpr uint32_t RADIUS_BLOCKS = 0U;
    static constexpr std::array<shared::ChunkCoordinate, 8U> EXPECTED{
        shared::ChunkCoordinate{.x = 0, .y = 0, .z = 0},
        shared::ChunkCoordinate{.x = 1, .y = 0, .z = 0},
        shared::ChunkCoordinate{.x = 0, .y = 1, .z = 0},
        shared::ChunkCoordinate{.x = 1, .y = 1, .z = 0},
        shared::ChunkCoordinate{.x = 0, .y = 0, .z = 1},
        shared::ChunkCoordinate{.x = 1, .y = 0, .z = 1},
        shared::ChunkCoordinate{.x = 0, .y = 1, .z = 1},
        shared::ChunkCoordinate{.x = 1, .y = 1, .z = 1},
    };

    std::vector<shared::ChunkCoordinate> const keys = shared::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );

    ASSERT_EQ(keys.size(), EXPECTED.size());
    EXPECT_TRUE(std::equal(keys.begin(), keys.end(), EXPECTED.begin()));
}

TEST(WorldCoverageTest, WrapsHorizontalSeamsAndDeduplicatesCanonicalKeys)
{
    static constexpr shared::PlayerPosition CENTER{.x = 0, .y = 0, .z = 8};
    static constexpr uint32_t RADIUS_BLOCKS = 0U;
    static constexpr std::array<shared::ChunkCoordinate, 4U> EXPECTED{
        shared::ChunkCoordinate{.x = 0, .y = 0, .z = 0},
        shared::ChunkCoordinate{.x = 4095, .y = 0, .z = 0},
        shared::ChunkCoordinate{.x = 0, .y = 4095, .z = 0},
        shared::ChunkCoordinate{.x = 4095, .y = 4095, .z = 0},
    };

    std::vector<shared::ChunkCoordinate> const keys = shared::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );

    ASSERT_EQ(keys.size(), EXPECTED.size());
    EXPECT_TRUE(std::equal(keys.begin(), keys.end(), EXPECTED.begin()));
}

TEST(WorldCoverageTest, FractionalPositionStaysInItsContainingChunk)
{
    static constexpr shared::PlayerPosition CENTER{
        .x = 16,
        .y = 16,
        .z = 16,
        .x_subcell = 1U,
        .y_subcell = 1U,
        .z_subcell = 1U,
    };
    static constexpr uint32_t RADIUS_BLOCKS = 0U;

    std::vector<shared::ChunkCoordinate> const keys = shared::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );

    ASSERT_EQ(keys.size(), 1U);
    EXPECT_EQ(keys.front(), (shared::ChunkCoordinate{.x = 1, .y = 1, .z = 1}));
}

TEST(WorldCoverageTest, FullRadiusClipsToTheFiniteVerticalWorld)
{
    static constexpr uint32_t RADIUS_BLOCKS = shared::MAX_SPHERE_RADIUS_BLOCKS;
    static constexpr shared::PlayerPosition FLOOR_CENTER{.x = 32'768, .y = 32'768, .z = 0};
    static constexpr shared::PlayerPosition CEILING_CENTER{
        .x = 32'768,
        .y = 32'768,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH - 1U),
    };

    std::vector<shared::ChunkCoordinate> const floor_keys = shared::requiredChunkKeys(
        FLOOR_CENTER,
        RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const ceiling_keys = shared::requiredChunkKeys(
        CEILING_CENTER,
        RADIUS_BLOCKS
    );

    ASSERT_FALSE(floor_keys.empty());
    ASSERT_FALSE(ceiling_keys.empty());
    EXPECT_TRUE(std::all_of(
        floor_keys.begin(),
        floor_keys.end(),
        [](shared::ChunkCoordinate const key) { return shared::WorldBounds::isValidChunk(key); }
    ));
    EXPECT_TRUE(std::all_of(
        ceiling_keys.begin(),
        ceiling_keys.end(),
        [](shared::ChunkCoordinate const key) { return shared::WorldBounds::isValidChunk(key); }
    ));
}

TEST(WorldCoverageTest, FullRadiusCountFitsTheFourPlayerSafeBound)
{
    static constexpr shared::PlayerPosition SPAWN{.x = 32'896, .y = 32'768, .z = 410};
    static constexpr uint32_t INITIAL_RADIUS_BLOCKS = 48U;
    static constexpr uint32_t MAX_RADIUS_BLOCKS = shared::MAX_SPHERE_RADIUS_BLOCKS;

    std::vector<shared::ChunkCoordinate> const initial_keys = shared::requiredChunkKeys(
        SPAWN,
        INITIAL_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const full_keys = shared::requiredChunkKeys(
        SPAWN,
        MAX_RADIUS_BLOCKS
    );

    EXPECT_EQ(initial_keys.size(), 212U);
    EXPECT_EQ(full_keys.size(), 37'364U);
    EXPECT_LE(full_keys.size(), shared::MAX_CHUNK_KEYS_PER_SPHERE_UPPER_BOUND);
    EXPECT_EQ(
        shared::MAX_FOUR_PLAYER_PROTECTED_CHUNK_KEYS,
        static_cast<uint64_t>(shared::MAX_CHUNK_KEYS_PER_SPHERE_UPPER_BOUND) * 4U
    );
}

TEST(WorldCoverageTest, RejectsInvalidVerticalPositionSubcell)
{
    static constexpr uint32_t RADIUS_BLOCKS = 0U;
    static constexpr shared::PlayerPosition INVALID{
        .x = 0,
        .y = 0,
        .z = 8,
        .z_subcell = shared::SUBCELLS_PER_CELL,
    };

    EXPECT_TRUE(shared::requiredChunkKeys(INVALID, RADIUS_BLOCKS).empty());
}

TEST(WorldCoverageTest, RejectsVerticalPositionOutsideWorld)
{
    static constexpr uint32_t RADIUS_BLOCKS = 0U;
    static constexpr shared::PlayerPosition INVALID{
        .x = 0,
        .y = 0,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH),
    };

    EXPECT_TRUE(shared::requiredChunkKeys(INVALID, RADIUS_BLOCKS).empty());
}

TEST(WorldCoverageTest, RejectsRadiusAboveTheSupportedSphere)
{
    static constexpr shared::PlayerPosition CENTER{.x = 0, .y = 0, .z = 8};
    static constexpr uint32_t INVALID_RADIUS_BLOCKS = shared::MAX_SPHERE_RADIUS_BLOCKS + 1U;

    EXPECT_TRUE(shared::requiredChunkKeys(CENTER, INVALID_RADIUS_BLOCKS).empty());
}
