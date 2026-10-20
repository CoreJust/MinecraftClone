#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>


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
    ASSERT_TRUE(world.publishPreview(COORDINATE, preview, WORLD_REVISION, WORLD_SEED));
    shared::SparseBlockQuery const previewed = world.queryBlock(BLOCK_COORDINATE);
    EXPECT_EQ(previewed.state, shared::GenerationState::Preview);
    EXPECT_FALSE(previewed.block.has_value());
    EXPECT_EQ(world.residentChunkCount(), 0U);

    shared::Chunk materialized{COORDINATE};
    ASSERT_TRUE(materialized.setBlock({.x = 0U, .y = 0U, .z = 0U}, shared::Block::Stone));
    ASSERT_TRUE(world.publishMaterializedChunk(materialized, WORLD_REVISION, WORLD_SEED));
    shared::SparseBlockQuery const known = world.queryBlock(BLOCK_COORDINATE);
    EXPECT_EQ(known.state, shared::GenerationState::Materialized);
    EXPECT_EQ(known.block, shared::Block::Stone);
    EXPECT_EQ(world.previewCount(), 0U);
}

TEST(SparseWorldTest, RejectsStaleMaterializationAndClearsOldRevision)
{
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 1, .y = 1, .z = 1};
    static constexpr shared::WorldCoordinate BLOCK_COORDINATE{16, 16, 16};
    static constexpr std::array<shared::ChunkCoordinate, 1U> PROTECTED{COORDINATE};
    static constexpr uint64_t CURRENT_REVISION = 9U;
    static constexpr uint64_t CURRENT_SEED = 73U;

    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{COORDINATE}, 1U, 42U));
    ASSERT_TRUE(world.setProtectedChunks(PROTECTED));
    world.setWorldIdentity(CURRENT_REVISION, CURRENT_SEED);
    EXPECT_EQ(world.residentChunkCount(), 0U);
    EXPECT_EQ(world.protectedChunkCount(), 0U);
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
    ASSERT_TRUE(world.publishPreview({.x = 1, .y = 1, .z = 0}, preview, 1U, 42U));

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

TEST(SparseWorldTest, ProtectedChunksCannotBeExplicitlyEvictedOrDisplaced)
{
    static constexpr uint64_t MAX_RESIDENT = 2U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND{.x = 2, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate THIRD{.x = 3, .y = 1, .z = 0};
    static constexpr shared::WorldCoordinate FIRST_BLOCK{16, 16, 0};
    static constexpr shared::WorldCoordinate SECOND_BLOCK{32, 16, 0};
    static constexpr std::array<shared::ChunkCoordinate, 2U> PROTECTED{FIRST, SECOND};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FIRST}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{SECOND}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.setBlock(FIRST_BLOCK, shared::Block::Dirt));
    ASSERT_TRUE(world.setBlock(SECOND_BLOCK, shared::Block::Snow));
    std::optional<shared::ChunkCoordinate> evicted_coordinate;

    ASSERT_TRUE(world.setProtectedChunks(PROTECTED));
    EXPECT_EQ(world.protectedChunkCount(), PROTECTED.size());
    EXPECT_TRUE(world.isChunkProtected(FIRST));
    EXPECT_TRUE(world.isChunkProtected(SECOND));
    EXPECT_FALSE(world.evict(FIRST));
    EXPECT_FALSE(world.publishMaterializedChunk(
        shared::Chunk{THIRD},
        WORLD_REVISION,
        WORLD_SEED,
        &evicted_coordinate
    ));

    EXPECT_FALSE(evicted_coordinate.has_value());
    EXPECT_EQ(world.residentChunkCount(), MAX_RESIDENT);
    EXPECT_EQ(world.blockAt(FIRST_BLOCK), shared::Block::Dirt);
    EXPECT_EQ(world.blockAt(SECOND_BLOCK), shared::Block::Snow);
}

TEST(SparseWorldTest, ReplacingProtectionDoesNotClearUnrelatedMutableChunks)
{
    static constexpr uint64_t MAX_RESIDENT = 3U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate PROTECTED{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate UNRELATED{.x = 2, .y = 1, .z = 0};
    static constexpr shared::WorldCoordinate UNRELATED_BLOCK{32, 16, 0};
    static constexpr std::array<shared::ChunkCoordinate, 1U> PROTECTED_KEYS{PROTECTED};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{PROTECTED},
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{UNRELATED},
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.setBlock(UNRELATED_BLOCK, shared::Block::Ice));

    ASSERT_TRUE(world.setProtectedChunks(PROTECTED_KEYS));

    EXPECT_EQ(world.residentChunkCount(), 2U);
    EXPECT_EQ(world.blockAt(UNRELATED_BLOCK), shared::Block::Ice);
}

TEST(SparseWorldTest, DuplicatePublicationPreservesExistingMutableChunkContents)
{
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 1, .y = 1, .z = 0};
    static constexpr shared::WorldCoordinate BLOCK{16, 16, 0};

    shared::SparseWorld world;
    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{COORDINATE},
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.setBlock(BLOCK, shared::Block::Grass));
    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{COORDINATE},
        WORLD_REVISION,
        WORLD_SEED
    ));

    EXPECT_EQ(world.blockAt(BLOCK), shared::Block::Grass);
}

TEST(SparseWorldTest, AdmissionReportsTheActualResidentEvictedByLru)
{
    static constexpr uint64_t MAX_RESIDENT = 2U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate PROTECTED{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate EVICTED{.x = 2, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate INCOMING{.x = 3, .y = 1, .z = 0};
    static constexpr std::array<shared::ChunkCoordinate, 1U> PROTECTED_KEYS{PROTECTED};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{PROTECTED},
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{EVICTED}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.setProtectedChunks(PROTECTED_KEYS));
    std::optional<shared::ChunkCoordinate> evicted_coordinate;

    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{INCOMING},
        WORLD_REVISION,
        WORLD_SEED,
        &evicted_coordinate
    ));

    ASSERT_TRUE(evicted_coordinate.has_value());
    EXPECT_EQ(*evicted_coordinate, EVICTED);
    EXPECT_NE(world.residentChunk(PROTECTED), nullptr);
    EXPECT_EQ(world.residentChunk(EVICTED), nullptr);
    EXPECT_NE(world.residentChunk(INCOMING), nullptr);
}

TEST(SparseWorldTest, AdmissionUpdatesLruOrderAfterResidentAccess)
{
    static constexpr uint64_t MAX_RESIDENT = 3U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND{.x = 2, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate THIRD{.x = 3, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate INCOMING{.x = 4, .y = 1, .z = 0};
    static constexpr shared::WorldCoordinate FIRST_BLOCK{16, 16, 0};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FIRST}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{SECOND}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{THIRD}, WORLD_REVISION, WORLD_SEED));

    static_cast<void>(world.blockAt(FIRST_BLOCK));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{INCOMING}, WORLD_REVISION, WORLD_SEED));

    EXPECT_NE(world.residentChunk(FIRST), nullptr);
    EXPECT_EQ(world.residentChunk(SECOND), nullptr);
    EXPECT_NE(world.residentChunk(THIRD), nullptr);
    EXPECT_NE(world.residentChunk(INCOMING), nullptr);
}

TEST(SparseWorldTest, AdmissionPreservesDeterministicLruWhenProtectionChanges)
{
    static constexpr uint64_t MAX_RESIDENT = 4U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND{.x = 2, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate THIRD{.x = 3, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate FOURTH{.x = 4, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate INCOMING{.x = 5, .y = 1, .z = 0};
    static constexpr std::array<shared::ChunkCoordinate, 3U> INITIAL_PROTECTION{
        FIRST,
        SECOND,
        THIRD,
    };
    static constexpr std::array<shared::ChunkCoordinate, 2U> REPLACED_PROTECTION{FIRST, THIRD};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FIRST}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{SECOND}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{THIRD}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FOURTH}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.setProtectedChunks(INITIAL_PROTECTION));
    ASSERT_TRUE(world.setProtectedChunks(REPLACED_PROTECTION));

    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{INCOMING}, WORLD_REVISION, WORLD_SEED));

    EXPECT_NE(world.residentChunk(FIRST), nullptr);
    EXPECT_EQ(world.residentChunk(SECOND), nullptr);
    EXPECT_NE(world.residentChunk(THIRD), nullptr);
    EXPECT_NE(world.residentChunk(FOURTH), nullptr);
    EXPECT_NE(world.residentChunk(INCOMING), nullptr);
}

TEST(SparseWorldTest, ProtectionUpdatesPreserveLruForAbsentAndReorderedDuplicateKeys)
{
    static constexpr uint64_t MAX_RESIDENT = 3U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate FIRST{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND{.x = 2, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate THIRD{.x = 3, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate FOURTH{.x = 4, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate ABSENT{.x = 5, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate FIFTH{.x = 6, .y = 1, .z = 0};
    static constexpr std::array<shared::ChunkCoordinate, 3U> INITIAL{FIRST, THIRD, ABSENT};
    static constexpr std::array<shared::ChunkCoordinate, 4U> REORDERED_DUPLICATES{
        ABSENT,
        THIRD,
        ABSENT,
        FIRST,
    };
    static constexpr std::array<shared::ChunkCoordinate, 2U> UNPROTECT_FIRST{ABSENT, THIRD};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FIRST}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{SECOND}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{THIRD}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.setProtectedChunks(INITIAL));
    ASSERT_TRUE(world.setProtectedChunks(INITIAL));
    ASSERT_TRUE(world.setProtectedChunks(REORDERED_DUPLICATES));
    EXPECT_EQ(world.protectedChunkCount(), MAX_RESIDENT);
    EXPECT_TRUE(world.isChunkProtected(FIRST));
    EXPECT_TRUE(world.isChunkProtected(THIRD));
    EXPECT_TRUE(world.isChunkProtected(ABSENT));

    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FOURTH}, WORLD_REVISION, WORLD_SEED));
    EXPECT_EQ(world.residentChunk(SECOND), nullptr);
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{ABSENT}, WORLD_REVISION, WORLD_SEED));
    EXPECT_EQ(world.residentChunk(FOURTH), nullptr);
    EXPECT_NE(world.residentChunk(THIRD), nullptr);
    EXPECT_FALSE(world.publishMaterializedChunk(shared::Chunk{FIFTH}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.setProtectedChunks(UNPROTECT_FIRST));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{FIFTH}, WORLD_REVISION, WORLD_SEED));

    EXPECT_FALSE(world.isChunkProtected(FIRST));
    EXPECT_TRUE(world.isChunkProtected(ABSENT));
    EXPECT_EQ(world.residentChunk(FIRST), nullptr);
    EXPECT_NE(world.residentChunk(ABSENT), nullptr);
    EXPECT_EQ(world.residentChunk(FOURTH), nullptr);
    EXPECT_NE(world.residentChunk(FIFTH), nullptr);
}

TEST(SparseWorldTest, MaterializedOnlyModeDoesNotGenerateUnknownChunks)
{
    static constexpr uint64_t MAX_RESIDENT = 2U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 1, .y = 1, .z = 0};
    static constexpr shared::WorldCoordinate BLOCK{16, 16, 0};

    shared::SparseWorld world(
        {.seed = WORLD_SEED, .max_resident_chunks = MAX_RESIDENT, .revision = WORLD_REVISION},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    shared::HeightTile preview{.coordinate = {.x = COORDINATE.x, .y = COORDINATE.y}};
    ASSERT_TRUE(world.publishPreview(COORDINATE, preview, WORLD_REVISION, WORLD_SEED));

    EXPECT_EQ(world.ensureChunk(COORDINATE), nullptr);
    EXPECT_EQ(world.residentChunkCount(), 0U);
    EXPECT_EQ(world.queryBlock(BLOCK).state, shared::GenerationState::Preview);
    EXPECT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{COORDINATE},
        WORLD_REVISION,
        WORLD_SEED
    ));
    EXPECT_EQ(world.blockAt(BLOCK), shared::Block::Air);
    EXPECT_EQ(world.previewCount(), 0U);
}

TEST(SparseWorldTest, ProtectionUpdateRejectsInvalidAndOverCapacitySetsAtomically)
{
    static constexpr uint64_t MAX_RESIDENT = 2U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr shared::ChunkCoordinate EXISTING{.x = 1, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate UNPROTECTED{.x = 2, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate INCOMING{.x = 3, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate OTHER{.x = 4, .y = 1, .z = 0};
    static constexpr shared::ChunkCoordinate INVALID{.x = 1, .y = 1, .z = 64};
    static constexpr std::array<shared::ChunkCoordinate, 1U> INITIAL{EXISTING};
    static constexpr std::array<shared::ChunkCoordinate, 2U> DUPLICATES{EXISTING, EXISTING};
    static constexpr std::array<shared::ChunkCoordinate, 3U> TOO_MANY{
        EXISTING,
        UNPROTECTED,
        OTHER,
    };
    static constexpr std::array<shared::ChunkCoordinate, 2U> INVALID_SET{UNPROTECTED, INVALID};

    shared::SparseWorld world({.max_resident_chunks = MAX_RESIDENT});
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{EXISTING}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{UNPROTECTED}, WORLD_REVISION, WORLD_SEED));
    ASSERT_TRUE(world.setProtectedChunks(INITIAL));
    ASSERT_TRUE(world.setProtectedChunks(DUPLICATES));
    EXPECT_EQ(world.protectedChunkCount(), 1U);

    EXPECT_FALSE(world.setProtectedChunks(TOO_MANY));
    EXPECT_TRUE(world.isChunkProtected(EXISTING));
    EXPECT_EQ(world.protectedChunkCount(), 1U);
    EXPECT_FALSE(world.setProtectedChunks(INVALID_SET));
    EXPECT_TRUE(world.isChunkProtected(EXISTING));
    EXPECT_EQ(world.protectedChunkCount(), 1U);

    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{INCOMING}, WORLD_REVISION, WORLD_SEED));
    EXPECT_NE(world.residentChunk(EXISTING), nullptr);
    EXPECT_EQ(world.residentChunk(UNPROTECTED), nullptr);
    EXPECT_NE(world.residentChunk(INCOMING), nullptr);
}

TEST(SparseWorldTest, RetainsEveryMandatoryMaximumSphereChunkWhileEvictingEligibleResidents)
{
    static constexpr shared::PlayerPosition CENTER{.x = 32'768, .y = 32'768, .z = 512};
    static constexpr uint32_t RADIUS_BLOCKS = shared::MAX_SPHERE_RADIUS_BLOCKS;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint64_t WORLD_SEED = shared::WorldConfiguration::SEED;
    static constexpr shared::ChunkCoordinate FIRST_ELIGIBLE{.x = 0, .y = 0, .z = 0};
    static constexpr shared::ChunkCoordinate SECOND_ELIGIBLE{.x = 1, .y = 0, .z = 0};

    std::vector<shared::ChunkCoordinate> const mandatory = shared::requiredChunkKeys(
        CENTER,
        RADIUS_BLOCKS
    );
    ASSERT_GT(mandatory.size(), 0U);
    ASSERT_EQ(mandatory.size(), 37'296U);
    ASSERT_EQ(
        std::find(mandatory.begin(), mandatory.end(), FIRST_ELIGIBLE),
        mandatory.end()
    );
    ASSERT_EQ(
        std::find(mandatory.begin(), mandatory.end(), SECOND_ELIGIBLE),
        mandatory.end()
    );

    uint64_t const capacity = static_cast<uint64_t>(mandatory.size()) + 1U;
    shared::SparseWorld world({
        .seed = WORLD_SEED,
        .max_resident_chunks = capacity,
        .revision = WORLD_REVISION,
    });

    ASSERT_TRUE(world.setProtectedChunks(mandatory));
    ASSERT_EQ(world.protectedChunkCount(), mandatory.size());
    for (shared::ChunkCoordinate const coordinate : mandatory) {
        ASSERT_TRUE(world.publishMaterializedChunk(
            shared::Chunk{coordinate},
            WORLD_REVISION,
            WORLD_SEED
        ));
    }
    ASSERT_EQ(world.residentChunkCount(), mandatory.size());
    shared::WorldCoordinate const first_mandatory_block{
        static_cast<int64_t>(mandatory.front().x) * shared::Chunk::SIDE_LENGTH,
        static_cast<int64_t>(mandatory.front().y) * shared::Chunk::SIDE_LENGTH,
        static_cast<int64_t>(mandatory.front().z) * shared::Chunk::SIDE_LENGTH,
    };
    ASSERT_EQ(
        world.queryBlock(first_mandatory_block).state,
        shared::GenerationState::Materialized
    );
    EXPECT_EQ(world.queryBlock(first_mandatory_block).block, shared::Block::Air);

    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{FIRST_ELIGIBLE},
        WORLD_REVISION,
        WORLD_SEED
    ));
    std::optional<shared::ChunkCoordinate> evicted_coordinate;
    ASSERT_TRUE(world.publishMaterializedChunk(
        shared::Chunk{SECOND_ELIGIBLE},
        WORLD_REVISION,
        WORLD_SEED,
        &evicted_coordinate
    ));

    ASSERT_TRUE(evicted_coordinate.has_value());
    EXPECT_EQ(*evicted_coordinate, FIRST_ELIGIBLE);
    EXPECT_EQ(world.residentChunk(FIRST_ELIGIBLE), nullptr);
    EXPECT_NE(world.residentChunk(SECOND_ELIGIBLE), nullptr);
    EXPECT_EQ(world.residentChunkCount(), capacity);
    EXPECT_LE(world.residentChunkCount(), world.maxResidentChunks());
    for (shared::ChunkCoordinate const coordinate : mandatory) {
        EXPECT_TRUE(world.isChunkProtected(coordinate));
        EXPECT_NE(world.residentChunk(coordinate), nullptr);
    }
}
