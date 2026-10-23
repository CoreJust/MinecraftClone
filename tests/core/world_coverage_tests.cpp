#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
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

TEST(WorldCoverageTest, ClosedSphereKeepsPointTangentCornerChunks)
{
    static constexpr shared::PlayerPosition CENTER{ .x = 32'768, .y = 32'768, .z = 1'072 };
    static constexpr uint32_t RADIUS_BLOCKS = 48U;
    static constexpr std::array<shared::ChunkCoordinate, 4U> EXPECTED{
        shared::ChunkCoordinate{ .x = 2'047, .y = 2'047, .z = 63 },
        shared::ChunkCoordinate{ .x = 2'048, .y = 2'047, .z = 63 },
        shared::ChunkCoordinate{ .x = 2'047, .y = 2'048, .z = 63 },
        shared::ChunkCoordinate{ .x = 2'048, .y = 2'048, .z = 63 },
    };

    std::vector<shared::ChunkCoordinate> const keys = shared::requiredChunkKeys(CENTER, RADIUS_BLOCKS);

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
        static_cast<uint64_t>(shared::MAX_CHUNK_KEYS_PER_NEAR_SPHERE_UPPER_BOUND) * 4U
    );
    EXPECT_EQ(shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS, MAX_RADIUS_BLOCKS);
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

TEST(WorldCoverageTest, RejectsEntityPositionAboveSupportedFlightHeight)
{
    static constexpr uint32_t RADIUS_BLOCKS = 0U;
    static constexpr shared::PlayerPosition INVALID{
        .x = 0,
        .y = 0,
        .z = 65'536,
    };

    EXPECT_TRUE(shared::requiredChunkKeys(INVALID, RADIUS_BLOCKS).empty());
}

TEST(WorldCoverageTest, KeepsTerrainCoverageClippedWhileHighEntityPositionsRemainValid)
{
    static constexpr uint32_t RADIUS_BLOCKS = 0U;
    static constexpr int32_t X = 32'768;
    static constexpr int32_t Y = 32'768;
    static constexpr shared::PlayerPosition LAST_TERRAIN_CELL{ .x = X, .y = Y, .z = 1'023 };
    static constexpr shared::PlayerPosition TERRAIN_CEILING{ .x = X, .y = Y, .z = 1'024 };
    static constexpr shared::PlayerPosition ABOVE_TERRAIN_BOUNDARY{ .x = X, .y = Y, .z = 1'025 };
    static constexpr shared::PlayerPosition VISUAL_ONLY{ .x = X, .y = Y, .z = 1'080 };
    static constexpr shared::PlayerPosition ABOVE_TERRAIN{ .x = X, .y = Y, .z = 1'400 };
    static constexpr shared::PlayerPosition ENTITY_LIMIT{ .x = X, .y = Y, .z = 65'535 };

    EXPECT_FALSE(shared::requiredChunkKeys(LAST_TERRAIN_CELL, RADIUS_BLOCKS).empty());
    EXPECT_FALSE(shared::requiredChunkKeys(TERRAIN_CEILING, RADIUS_BLOCKS).empty());
    EXPECT_TRUE(shared::requiredChunkKeys(ABOVE_TERRAIN_BOUNDARY, RADIUS_BLOCKS).empty());
    std::vector<shared::ChunkCoordinate> const ceiling_sphere = shared::requiredChunkKeys(
        TERRAIN_CEILING,
        shared::MAX_SPHERE_RADIUS_BLOCKS
    );
    EXPECT_FALSE(ceiling_sphere.empty());
    EXPECT_TRUE(std::all_of(ceiling_sphere.begin(), ceiling_sphere.end(), [](shared::ChunkCoordinate const key) {
        return key.z < static_cast<int32_t>(shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH);
    }));
    EXPECT_TRUE(shared::requiredChunkKeys(VISUAL_ONLY, shared::ACTUAL_TERRAIN_RADIUS_BLOCKS).empty());
    EXPECT_FALSE(shared::requiredChunkKeys(VISUAL_ONLY, shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS).empty());
    EXPECT_FALSE(shared::materializedInterestChunkKeys(VISUAL_ONLY).empty());
    EXPECT_TRUE(shared::requiredChunkKeys(ABOVE_TERRAIN, RADIUS_BLOCKS).empty());
    EXPECT_TRUE(shared::requiredChunkKeys(ABOVE_TERRAIN, shared::MAX_SPHERE_RADIUS_BLOCKS).empty());
    EXPECT_TRUE(shared::materializedInterestChunkKeys(ABOVE_TERRAIN).empty());
    EXPECT_TRUE(shared::requiredChunkKeys(ENTITY_LIMIT, RADIUS_BLOCKS).empty());
}

TEST(WorldCoverageTest, RejectsRadiusAboveTheSupportedSphere)
{
    static constexpr shared::PlayerPosition CENTER{.x = 0, .y = 0, .z = 8};
    static constexpr uint32_t INVALID_RADIUS_BLOCKS = shared::MAX_SPHERE_RADIUS_BLOCKS + 1U;

    EXPECT_TRUE(shared::requiredChunkKeys(CENTER, INVALID_RADIUS_BLOCKS).empty());
}

TEST(WorldCoverageTest, MaterializedInterestContainsMandatorySphereAndBoundedLead)
{
    static constexpr shared::PlayerPosition CENTER{.x = 0, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate LEAD_KEY{.x = 4, .y = 0, .z = 0};
    static constexpr auto KEY_ORDER = [](shared::ChunkCoordinate const left, shared::ChunkCoordinate const right) {
        if (left.z != right.z) {
            return left.z < right.z;
        }
        if (left.y != right.y) {
            return left.y < right.y;
        }
        return left.x < right.x;
    };

    std::vector<shared::ChunkCoordinate> const mandatory_keys = shared::requiredChunkKeys(
        CENTER,
        shared::NEAR_TERRAIN_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const interest_keys = shared::materializedInterestChunkKeys(CENTER);

    ASSERT_FALSE(mandatory_keys.empty());
    ASSERT_GE(interest_keys.size(), mandatory_keys.size());
    EXPECT_TRUE(std::ranges::includes(interest_keys, mandatory_keys, KEY_ORDER));
    EXPECT_FALSE(std::binary_search(mandatory_keys.begin(), mandatory_keys.end(), LEAD_KEY, KEY_ORDER));
    EXPECT_TRUE(std::ranges::find(interest_keys, LEAD_KEY) != interest_keys.end());
    EXPECT_LE(
        interest_keys.size(),
        shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND
    );
}

TEST(WorldCoverageTest, MaterializedInterestWrapsHorizontalSeamsAndClipsVerticalEdges)
{
    static constexpr shared::PlayerPosition SEAM_CENTER{.x = 0, .y = 0, .z = 8};
    static constexpr shared::PlayerPosition FLOOR_CENTER{.x = 32'768, .y = 32'768, .z = 0};
    static constexpr shared::PlayerPosition CEILING_CENTER{
        .x = 32'768,
        .y = 32'768,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH - 1U),
    };

    std::vector<shared::ChunkCoordinate> const seam_keys = shared::materializedInterestChunkKeys(SEAM_CENTER);
    std::vector<shared::ChunkCoordinate> const floor_keys = shared::materializedInterestChunkKeys(FLOOR_CENTER);
    std::vector<shared::ChunkCoordinate> const ceiling_keys = shared::materializedInterestChunkKeys(CEILING_CENTER);
    std::array<std::vector<shared::ChunkCoordinate> const*, 2U> const edge_keys{
        &floor_keys,
        &ceiling_keys,
    };

    EXPECT_TRUE(std::ranges::find(seam_keys, (shared::ChunkCoordinate{.x = 4095, .y = 0, .z = 0}))
        != seam_keys.end());
    EXPECT_TRUE(std::ranges::find(seam_keys, (shared::ChunkCoordinate{.x = 0, .y = 4095, .z = 0}))
        != seam_keys.end());
    EXPECT_TRUE(std::ranges::find(seam_keys, (shared::ChunkCoordinate{.x = 4095, .y = 4095, .z = 0}))
        != seam_keys.end());
    for (std::vector<shared::ChunkCoordinate> const* keys : edge_keys) {
        ASSERT_FALSE(keys->empty());
        EXPECT_LE(keys->size(), shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND);
        EXPECT_TRUE(std::all_of(
            keys->begin(),
            keys->end(),
            [](shared::ChunkCoordinate const key) { return shared::WorldBounds::isValidChunk(key); }
        ));
    }
}

TEST(WorldCoverageTest, MaterializedInterestUsesSubcellPositionAtLeadBoundary)
{
    static constexpr shared::PlayerPosition BEFORE_LEAD_BOUNDARY{
        .x = 15,
        .y = 0,
        .z = 0,
        .x_subcell = shared::SUBCELLS_PER_CELL - 1U,
    };
    static constexpr shared::PlayerPosition AT_LEAD_BOUNDARY{.x = 16, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate TANGENT_KEY{.x = 6, .y = 0, .z = 0};

    std::vector<shared::ChunkCoordinate> const before_keys =
        shared::materializedInterestChunkKeys(BEFORE_LEAD_BOUNDARY);
    std::vector<shared::ChunkCoordinate> const at_keys = shared::materializedInterestChunkKeys(AT_LEAD_BOUNDARY);

    EXPECT_TRUE(std::ranges::find(before_keys, TANGENT_KEY) == before_keys.end());
    EXPECT_TRUE(std::ranges::find(at_keys, TANGENT_KEY) != at_keys.end());
}

TEST(WorldCoverageTest, MaterializedInterestBoundsMatchTheFourPlayerMemoryCeiling)
{
    static constexpr uint32_t EXPECTED_RADIUS_BLOCKS = 80U;
    static constexpr uint32_t EXPECTED_SINGLE_PLAYER_BOUND = 1'024U;

    EXPECT_EQ(shared::ACTUAL_TERRAIN_RADIUS_BLOCKS, 640U);
    EXPECT_EQ(shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS, 320U);
    EXPECT_EQ(shared::MATERIALIZED_PREFETCH_MARGIN_BLOCKS, 32U);
    EXPECT_EQ(shared::MAX_MATERIALIZED_INTEREST_RADIUS_BLOCKS, EXPECTED_RADIUS_BLOCKS);
    EXPECT_EQ(
        shared::MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND,
        EXPECTED_SINGLE_PLAYER_BOUND
    );
    EXPECT_EQ(
        shared::MAX_FOUR_PLAYER_MATERIALIZED_INTEREST_CHUNK_KEYS,
        static_cast<uint64_t>(EXPECTED_SINGLE_PLAYER_BOUND) * 4U
    );
    EXPECT_EQ(shared::MAX_CHUNK_KEYS_PER_SPHERE_UPPER_BOUND, 43'016U);
    EXPECT_EQ(shared::MAX_CHUNK_KEYS_PER_NEAR_SPHERE_UPPER_BOUND, 512U);
    EXPECT_EQ(shared::MAX_FOUR_PLAYER_PROTECTED_CHUNK_KEYS, 2'048U);
    EXPECT_EQ(shared::MAX_FOUR_PLAYER_MATERIALIZED_INTEREST_CHUNK_KEYS, 4'096U);
}

TEST(WorldCoverageTest, FortyChunkTerrainCoverageIsHorizontalFullDetailSquare)
{
    auto const keys = shared::requiredTerrainColumns({.x = 0, .y = 0, .z = 463}, 640U);
    ASSERT_EQ(keys.size(), 81U * 81U);
    EXPECT_TRUE(std::ranges::all_of(keys, [](auto const key) { return key.z == 0; }));
    EXPECT_NE(std::ranges::find(keys, shared::ChunkCoordinate{40, 40, 0}), keys.end());
    EXPECT_NE(std::ranges::find(keys, shared::ChunkCoordinate{4056, 4056, 0}), keys.end());
    EXPECT_EQ(keys, shared::requiredTerrainColumns({.x = 0, .y = 0, .z = 60'000}, 640U));
    EXPECT_EQ(shared::requiredTerrainColumns({.x = 0, .y = 0, .z = 463}, 768U).size(), 97U * 97U);
    EXPECT_TRUE(shared::requiredTerrainColumns({.x = 0, .y = 0, .z = 463}, 769U).empty());
}
