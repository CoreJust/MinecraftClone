#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>
#include <testsupport/TerrainFixtures.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

shared::SurfaceTile makeCanonicalSurface(
    shared::ChunkCoordinate const coordinate,
    shared::TerrainIdentity identity,
    uint16_t const height = 385U
)
{
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
    heights.fill(height);
    return testsupport::makeSurfaceTile(
        { .x = coordinate.x, .y = coordinate.y },
        std::move(identity),
        heights
    );
}

shared::Chunk makeCanonicalChunk(
    shared::SurfaceTile const& surface,
    shared::ChunkCoordinate const coordinate
)
{
    shared::Chunk::Blocks blocks{};
    for (uint8_t z = 0U; z < shared::Chunk::SIDE_LENGTH; ++z) {
        uint32_t const world_z = static_cast<uint32_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH + z;
        for (uint8_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
                uint32_t const column_index = static_cast<uint32_t>(y) * shared::Chunk::SIDE_LENGTH + x;
                uint32_t const block_index = static_cast<uint32_t>(z) * shared::Chunk::FACE_BLOCK_COUNT
                    + static_cast<uint32_t>(y) * shared::Chunk::SIDE_LENGTH
                    + x;
                blocks[block_index] = surface.columns[column_index].materialAt(
                    static_cast<uint16_t>(world_z),
                    surface.block_sea_level
                );
            }
        }
    }
    return shared::Chunk{ coordinate, blocks };
}

} // namespace

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
    static constexpr shared::ChunkCoordinate INVALID{
        .x = 1, .y = 1,
        .z = static_cast<int32_t>(shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH),
    };
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

TEST(SparseWorldTest, SurfaceMapProofRequiresMatchingIdentityAndResidentCanonicalHChunk)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr uint16_t HIGHEST_SOLID_Z = 384U;
    static constexpr shared::ChunkCoordinate XY_TILE{ .x = 1, .y = 1, .z = 0 };
    static constexpr shared::ChunkCoordinate H_CHUNK{ .x = 1, .y = 1, .z = 24 };
    static constexpr shared::WorldCoordinate TOP_BLOCK{ .x = 16, .y = 16, .z = HIGHEST_SOLID_Z };
    static constexpr shared::WorldCoordinate UNMATERIALIZED_BLOCK{ .x = 16, .y = 16, .z = 900 };

    shared::TerrainIdentity const identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    std::unique_ptr<shared::SurfaceTile const> const surface_owner{
        new shared::SurfaceTile(makeCanonicalSurface(XY_TILE, identity))
    };
    auto const& surface = *surface_owner;
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 4U,
            .revision = WORLD_REVISION,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );

    ASSERT_TRUE(world.setTerrainIdentity(identity));
    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    EXPECT_EQ(world.residentChunkCount(), 0U);
    EXPECT_EQ(world.queryBlock(TOP_BLOCK).state, shared::GenerationState::Unknown);

    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, H_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));

    auto const verified = world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->highest_solid_z, HIGHEST_SOLID_Z);
    EXPECT_EQ(verified->material_at_highest_solid, shared::Block::Grass);
    EXPECT_EQ(world.queryBlock(UNMATERIALIZED_BLOCK).state, shared::GenerationState::Unknown);

    ASSERT_TRUE(world.evict(H_CHUNK));
    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    EXPECT_EQ(world.queryBlock(TOP_BLOCK).state, shared::GenerationState::Unknown);
    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, H_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));
    EXPECT_TRUE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());

    generation::GenerationConfiguration other_configuration;
    other_configuration.seed = WORLD_SEED;
    other_configuration.resolution = 512U;
    shared::TerrainIdentity wrong_identity = testsupport::makeTerrainIdentity(
        WORLD_SEED,
        WORLD_REVISION,
        generation::configurationIdentity(other_configuration)
    );
    auto const wrong_surface = std::make_unique<shared::SurfaceTile>(surface);
    wrong_surface->identity = std::move(wrong_identity);
    EXPECT_FALSE(world.publishSurfaceMetadata(*wrong_surface));
}

TEST(SparseWorldTest, AnyChunkMutationInvalidatesSurfaceMetadataProof)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr shared::ChunkCoordinate XY_TILE{ .x = 1, .y = 1, .z = 0 };
    static constexpr shared::ChunkCoordinate H_CHUNK{ .x = 1, .y = 1, .z = 24 };
    static constexpr shared::WorldCoordinate TOP_BLOCK{ .x = 16, .y = 16, .z = 384 };
    static constexpr shared::WorldCoordinate EDITED_BLOCK{ .x = 16, .y = 16, .z = 385 };

    shared::TerrainIdentity const identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    shared::SurfaceTile const surface = makeCanonicalSurface(XY_TILE, identity);
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 4U,
            .revision = WORLD_REVISION,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, H_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());

    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Snow));

    EXPECT_EQ(world.surfaceMetadataTileCount(), 0U);
    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    EXPECT_EQ(world.blockAt(EDITED_BLOCK), shared::Block::Snow);
}

TEST(SparseWorldTest, KnownNonAirAboveCanonicalSurfaceKeepsMapColumnUnknown)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr shared::ChunkCoordinate XY_TILE{ .x = 1, .y = 1, .z = 0 };
    static constexpr shared::ChunkCoordinate H_CHUNK{ .x = 1, .y = 1, .z = 24 };
    static constexpr shared::ChunkCoordinate HIGHER_CHUNK{ .x = 1, .y = 1, .z = 25 };
    static constexpr shared::WorldCoordinate TOP_BLOCK{ .x = 16, .y = 16, .z = 384 };

    shared::TerrainIdentity const identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    shared::SurfaceTile const surface = makeCanonicalSurface(XY_TILE, identity);
    shared::Chunk higher{ HIGHER_CHUNK };
    ASSERT_TRUE(higher.setBlock({ .x = 0U, .y = 0U, .z = 0U }, shared::Block::Stone));
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 4U,
            .revision = WORLD_REVISION,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, H_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.publishMaterializedChunk(higher, WORLD_REVISION, WORLD_SEED));

    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    EXPECT_EQ(world.surfaceMetadataTileCount(), 0U);
}

TEST(SparseWorldTest, DirectMutationOfAnyResidentZChunkInvalidatesTheSurfaceProof)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr shared::ChunkCoordinate XY_TILE{ .x = 1, .y = 1, .z = 0 };
    static constexpr shared::ChunkCoordinate H_CHUNK{ .x = 1, .y = 1, .z = 24 };
    static constexpr shared::ChunkCoordinate HIGHER_CHUNK{ .x = 1, .y = 1, .z = 25 };
    static constexpr shared::WorldCoordinate TOP_BLOCK{ .x = 16, .y = 16, .z = 384 };

    shared::TerrainIdentity const identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    shared::SurfaceTile const surface = makeCanonicalSurface(XY_TILE, identity);
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 4U,
            .revision = WORLD_REVISION,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, H_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, HIGHER_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());

    shared::Chunk* const higher_chunk = world.ensureChunk(HIGHER_CHUNK);
    ASSERT_NE(higher_chunk, nullptr);
    ASSERT_TRUE(higher_chunk->setBlock({ .x = 0U, .y = 0U, .z = 0U }, shared::Block::Stone));

    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
    EXPECT_EQ(world.surfaceMetadataTileCount(), 0U);
}

TEST(SparseWorldTest, ChangingTerrainIdentityClearsExistingSurfaceMetadata)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr shared::ChunkCoordinate XY_TILE{ .x = 1, .y = 1, .z = 0 };
    static constexpr shared::ChunkCoordinate H_CHUNK{ .x = 1, .y = 1, .z = 24 };
    static constexpr shared::WorldCoordinate TOP_BLOCK{ .x = 16, .y = 16, .z = 384 };

    shared::TerrainIdentity identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    shared::SurfaceTile const surface = makeCanonicalSurface(XY_TILE, identity);
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 4U,
            .revision = WORLD_REVISION,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(world.publishMaterializedChunk(
        makeCanonicalChunk(surface, H_CHUNK),
        WORLD_REVISION,
        WORLD_SEED
    ));
    ASSERT_TRUE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());

    generation::GenerationConfiguration other_configuration;
    other_configuration.seed = WORLD_SEED;
    other_configuration.resolution = 512U;
    identity.configuration_identity = generation::configurationIdentity(other_configuration);
    ASSERT_TRUE(world.setTerrainIdentity(identity));

    EXPECT_EQ(world.surfaceMetadataTileCount(), 0U);
    EXPECT_FALSE(world.verifiedSurfaceColumn(TOP_BLOCK.x, TOP_BLOCK.y).has_value());
}

TEST(SparseWorldTest, SurfaceMetadataIsBoundedAndExplicitlyRemovable)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr int32_t FIRST_TILE_X = 0;
    static constexpr int32_t LAST_INSERTED_TILE_X = static_cast<int32_t>(shared::MAX_SURFACE_METADATA_TILES);
    static constexpr int32_t TILE_Y = 1;
    static constexpr shared::ChunkCoordinate EVICTED_TILE{ .x = FIRST_TILE_X, .y = TILE_Y, .z = 0 };
    static constexpr shared::ChunkCoordinate LAST_TILE{ .x = LAST_INSERTED_TILE_X, .y = TILE_Y, .z = 0 };

    shared::TerrainIdentity const identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 1U,
            .revision = WORLD_REVISION,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    for (int32_t tile_x = FIRST_TILE_X; tile_x <= LAST_INSERTED_TILE_X; ++tile_x) {
        shared::SurfaceTile const surface = makeCanonicalSurface(
            { .x = tile_x, .y = TILE_Y, .z = 0 },
            identity
        );
        ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    }

    EXPECT_EQ(world.surfaceMetadataTileCount(), shared::MAX_SURFACE_METADATA_TILES);
    EXPECT_EQ(world.residentChunkCount(), 0U);
    EXPECT_EQ(world.queryBlock({ .x = 16, .y = 16, .z = 384 }).state, shared::GenerationState::Unknown);
    EXPECT_FALSE(world.removeSurfaceMetadata(EVICTED_TILE));
    EXPECT_TRUE(world.removeSurfaceMetadata(LAST_TILE));
    EXPECT_EQ(world.surfaceMetadataTileCount(), shared::MAX_SURFACE_METADATA_TILES - 1U);
    EXPECT_FALSE(world.removeSurfaceMetadata({ .x = 1, .y = 1, .z = 1 }));
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

TEST(SparseWorldTest, AuthoritativeProfilesCoverEveryVoxelWithoutDenseClientGeneration)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface({.x = 4, .y = 5, .z = 0}, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    EXPECT_EQ(world.queryBlock({64, 80, 900}).state, shared::GenerationState::Unknown);
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    EXPECT_EQ(world.residentChunkCount(), 0U);
    for (uint16_t z = 0U; z < shared::WorldExtent::DEPTH; ++z) {
        auto const query = world.queryBlock({64, 80, z});
        ASSERT_EQ(query.state, shared::GenerationState::Materialized);
        EXPECT_EQ(query.block, surface.columns[0].materialAt(z, surface.block_sea_level));
    }
    for (int32_t z = 0; z < 64; ++z) {
        shared::ChunkCoordinate const key{4, 5, z};
        ASSERT_TRUE(world.hasAuthoritativeChunk(key));
        auto const expected = makeCanonicalChunk(surface, key);
        auto const* actual = world.ensureChunk(key);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(actual->contentIdentity().content_hash, expected.contentIdentity().content_hash);
    }
    EXPECT_LE(world.residentChunkCount(), 2U);
    auto const authoritative_column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(authoritative_column.has_value());
    if (!authoritative_column) {
        return;
    }
    EXPECT_EQ(authoritative_column->dense_section_mask, shared::DenseSectionMask{});
    EXPECT_EQ(authoritative_column->dense_blocks, nullptr);
}

TEST(SparseWorldTest, V9SandTopRemainsSolidAcrossAuthorityQueryAndChunkMaterialization)
{
    generation::GenerationConfiguration const configuration{
        .seed = 42U,
        .model_tag = generation::TerrainModelTag::V9,
    };
    shared::TerrainIdentity const identity{
        .terrain_version = generation::terrainVersion(generation::TerrainModelTag::V9),
        .revision = 1U,
        .seed = configuration.seed,
        .configuration_identity = generation::configurationIdentity(configuration),
    };
    shared::SurfaceTile surface;
    surface.coordinate = {.x = 4, .y = 5, .z = 0};
    surface.block_sea_level = generation::V9_BLOCK_SEA_LEVEL;
    surface.max_height = generation::V9_MAX_HEIGHT;
    surface.identity = identity;
    for (shared::TerrainColumn& column : surface.columns) {
        column.highest_solid_z = static_cast<uint16_t>(surface.block_sea_level);
        column.water_height_z = static_cast<uint16_t>(surface.block_sea_level);
        column.geological_elevation_km = 0.001;
        column.temperature_celsius = 12.0;
        column.precipitation = 0.3;
        column.top_material = shared::Block::Grass;
        column.ground_material = shared::Block::Dirt;
        column.ground_first_z = static_cast<uint16_t>(surface.block_sea_level - 4U);
        column.ground_end_z = static_cast<uint16_t>(surface.block_sea_level);
        column.source_l0_elevation_m = 100.0;
        column.source_biome_id = 4U;
    }
    surface.columns.front().top_material = shared::Block::Sand;
    surface.columns.front().source_biome_id = 5U;
    ASSERT_TRUE(shared::isValidSurfaceTile(surface));

    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    EXPECT_EQ(world.queryBlock({64, 80, surface.block_sea_level}).block, shared::Block::Sand);
    shared::Chunk const* const chunk = world.ensureChunk({
        4,
        5,
        static_cast<int32_t>(surface.block_sea_level / shared::Chunk::SIDE_LENGTH),
    });
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->blockAt({.x = 0U, .y = 0U, .z = 0U}), shared::Block::Sand);
}

TEST(SparseWorldTest, V10AuthoritativeProfilesRetainSubmergedSandAndDirtControls)
{
    generation::GenerationConfiguration configuration{};
    configuration.model_tag = generation::TerrainModelTag::V10;
    shared::TerrainIdentity const identity{
        .terrain_version = generation::terrainVersion(configuration.model_tag),
        .revision = 1U,
        .seed = configuration.seed,
        .configuration_identity = generation::configurationIdentity(configuration),
    };
    shared::SurfaceTile surface{
        .coordinate = {.x = 4, .y = 5, .z = 0},
        .block_sea_level = generation::V10_BLOCK_SEA_LEVEL,
        .max_height = generation::V10_MAX_HEIGHT,
        .identity = identity,
    };
    auto const set_column = [](shared::TerrainColumn& column, uint16_t const highest_solid_z,
                               double const elevation_m, uint16_t const water_height_z,
                               bool const water_present, bool const source_native_ocean,
                               bool const source_native_coast_adjacent, shared::Block const top_material) {
        bool const submerged = highest_solid_z < generation::V10_BLOCK_SEA_LEVEL;
        uint32_t const ground_first_z = submerged
            ? generation::V10_BLOCK_SEA_LEVEL - 4U
            : (highest_solid_z > 4U ? static_cast<uint32_t>(highest_solid_z) - 4U : 1U);
        uint32_t const ground_end_z = std::max(
            submerged ? static_cast<uint32_t>(highest_solid_z) + 1U : highest_solid_z,
            ground_first_z
        );
        column = {
            .highest_solid_z = highest_solid_z,
            .water_height_z = water_height_z,
            .water_present = water_present,
            .submerged = submerged,
            .geological_elevation_km = elevation_m / 1'000.0,
            .temperature_celsius = 12.0,
            .precipitation = 0.3,
            .top_material = top_material,
            .ground_material = shared::Block::Dirt,
            .ground_first_z = static_cast<uint16_t>(ground_first_z),
            .ground_end_z = static_cast<uint16_t>(ground_end_z),
            .source_l0_elevation_m = elevation_m,
            .source_biome_id = 4U,
            .source_native_coast_adjacent = source_native_coast_adjacent,
            .source_native_ocean = source_native_ocean,
        };
    };
    for (shared::TerrainColumn& column : surface.columns) {
        set_column(column, generation::V10_BLOCK_SEA_LEVEL, 0.0,
            static_cast<uint16_t>(generation::V10_BLOCK_SEA_LEVEL), false, false, false,
            shared::Block::Grass);
    }
    set_column(surface.columns[0U], 509U, -3.0,
        static_cast<uint16_t>(generation::V10_BLOCK_SEA_LEVEL), true, true, true, shared::Block::Sand);
    set_column(surface.columns[1U], 450U, -62.0,
        static_cast<uint16_t>(generation::V10_BLOCK_SEA_LEVEL), true, true, true, shared::Block::Dirt);
    set_column(surface.columns[2U], 600U, 88.0, 620U, true, false, false, shared::Block::Dirt);
    ASSERT_TRUE(shared::isValidSurfaceTile(surface));
    EXPECT_EQ(surface.columns[0U].materialAt(509U, surface.block_sea_level), shared::Block::Sand);

    shared::SparseWorld world({.seed = configuration.seed, .max_resident_chunks = 3U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));

    struct SurfaceCase final {
        uint32_t local_x = 0U;
        uint16_t highest_solid_z = 0U;
        shared::Block top_material = shared::Block::Air;
        shared::Block material_at_top = shared::Block::Air;
    };
    std::array<SurfaceCase, 3U> const cases{{
        {0U, 509U, shared::Block::Sand, shared::Block::Sand},
        {1U, 450U, shared::Block::Dirt, shared::Block::Stone},
        {2U, 600U, shared::Block::Dirt, shared::Block::Dirt},
    }};
    for (SurfaceCase const& surface_case : cases) {
        int64_t const world_x = 64 + surface_case.local_x;
        int64_t const world_y = 80;
        shared::TerrainColumn const& source = surface.columns[surface_case.local_x];
        EXPECT_EQ(source.highest_solid_z, surface_case.highest_solid_z);
        EXPECT_EQ(source.top_material, surface_case.top_material);
        EXPECT_EQ(source.materialAt(surface_case.highest_solid_z, surface.block_sea_level),
            surface_case.material_at_top);

        shared::SparseBlockQuery const query = world.queryBlock(
            {world_x, world_y, surface_case.highest_solid_z});
        EXPECT_EQ(query.state, shared::GenerationState::Materialized);
        EXPECT_EQ(query.block, surface_case.material_at_top);
        EXPECT_EQ(world.blockAt({world_x, world_y, surface_case.highest_solid_z}),
            surface_case.material_at_top);

        auto const verified = world.verifiedSurfaceColumn(world_x, world_y);
        ASSERT_TRUE(verified.has_value());
        if (!verified) {
            continue;
        }
        EXPECT_EQ(verified->highest_solid_z, surface_case.highest_solid_z);
        EXPECT_EQ(verified->material_at_highest_solid, surface_case.material_at_top);
        auto const authoritative = world.authoritativeColumn(world_x, world_y);
        ASSERT_TRUE(authoritative.has_value());
        if (!authoritative) {
            continue;
        }
        EXPECT_EQ(authoritative->top_material, surface_case.top_material);

        shared::ChunkCoordinate const chunk_coordinate{
            .x = 4,
            .y = 5,
            .z = static_cast<int32_t>(surface_case.highest_solid_z / shared::Chunk::SIDE_LENGTH),
        };
        shared::Chunk const* const chunk = world.ensureChunk(chunk_coordinate);
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(chunk->blockAt({
            .x = static_cast<uint8_t>(surface_case.local_x),
            .y = 0U,
            .z = static_cast<uint8_t>(surface_case.highest_solid_z % shared::Chunk::SIDE_LENGTH),
        }), surface_case.material_at_top);
    }

    shared::SparseWorld metadata_world({.seed = configuration.seed, .max_resident_chunks = 1U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(metadata_world.setTerrainIdentity(identity));
    ASSERT_TRUE(metadata_world.publishSurfaceMetadata(surface));
    ASSERT_TRUE(metadata_world.publishMaterializedChunk(
        makeCanonicalChunk(surface, {.x = 4, .y = 5, .z = 31}), 1U, configuration.seed));
    auto const metadata_verified = metadata_world.verifiedSurfaceColumn(64, 80);
    ASSERT_TRUE(metadata_verified.has_value());
    EXPECT_EQ(metadata_verified->highest_solid_z, 509U);
    EXPECT_EQ(metadata_verified->material_at_highest_solid, shared::Block::Sand);
}

TEST(SparseWorldTest, AuthoritativeProfilesPreserveDenseEditsAndInvalidateOnIdentityChange)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface({.x = 4, .y = 5, .z = 0}, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const before = world.authoritativeSurfaceRevision();
    ASSERT_TRUE(world.setBlock({64, 80, 900}, shared::Block::Snow));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(column.has_value());
    EXPECT_EQ(column->base_highest_solid_z, surface.columns[0].highest_solid_z);
    EXPECT_EQ(column->dense_section_mask, shared::DenseSectionMask{1U} << 56U);
    EXPECT_EQ(column->highest_solid_z, 900U);
    EXPECT_EQ(column->materialAt(900), shared::Block::Snow);
    EXPECT_EQ(column->materialAt(899), shared::Block::Air);
    EXPECT_EQ(world.queryBlock({64, 80, 900}).block, shared::Block::Snow);
    EXPECT_EQ(world.queryBlock({65, 80, 900}).block, shared::Block::Air);
    EXPECT_GT(world.authoritativeSurfaceRevision(), before);
    EXPECT_TRUE(world.evict({4, 5, 56}));
    EXPECT_EQ(world.queryBlock({64, 80, 900}).block, shared::Block::Snow);
    auto* const rematerialized = world.ensureChunk({4, 5, 56});
    ASSERT_NE(rematerialized, nullptr);
    EXPECT_EQ(rematerialized->blockAt({0, 0, 4}), shared::Block::Snow);
    world.setWorldIdentity(2U, 42U);
    EXPECT_FALSE(world.authoritativeColumn(64, 80).has_value());
    EXPECT_EQ(world.queryBlock({64, 80, 900}).state, shared::GenerationState::Unknown);
}

TEST(SparseWorldTest, DenseSectionMasksPreserveUpperEditsAcrossEvictionAndIdentityChange)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface({4, 5, 0}, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 4U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    shared::DenseSectionMask expected;
    for (uint32_t const section : {0U, 63U, 64U, 127U}) {
        expected.set(section);
        int64_t const z = section * shared::Chunk::SIDE_LENGTH;
        ASSERT_TRUE(world.setBlock({64, 80, z}, shared::Block::Snow));
    }
    auto const column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(column.has_value());
    EXPECT_EQ(column->dense_section_mask, expected);
    EXPECT_EQ(column->highest_solid_z, 2'032U);
    for (uint32_t const section : {0U, 63U, 64U, 127U}) {
        auto const z = static_cast<uint16_t>(section * shared::Chunk::SIDE_LENGTH);
        EXPECT_EQ(column->materialAt(z), shared::Block::Snow);
        ASSERT_TRUE(world.evict({4, 5, static_cast<int32_t>(section)}));
    }
    auto const unloaded = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(unloaded.has_value());
    EXPECT_EQ(unloaded->dense_section_mask, expected);
    EXPECT_EQ(unloaded->content_hash, column->content_hash);
    for (uint32_t const section : {0U, 63U, 64U, 127U}) {
        auto* const chunk = world.ensureChunk({4, 5, static_cast<int32_t>(section)});
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(chunk->blockAt({0U, 0U, 0U}), shared::Block::Snow);
    }
    world.setWorldIdentity(2U, 42U);
    EXPECT_FALSE(world.authoritativeColumn(64, 80).has_value());
}

TEST(SparseWorldTest, DenseTileHashesCoverAllSectionsInAscendingOrderAndRestoreAfterMutation)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 4U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const baseline = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(baseline.has_value());
    uint64_t expected_hash = 14'695'981'039'346'656'037ULL;
    for (uint32_t const section : {0U, 63U, 64U, 127U}) {
        auto* const chunk = world.ensureChunk({4, 5, static_cast<int32_t>(section)});
        ASSERT_NE(chunk, nullptr);
        auto const original = chunk->blockAt({0U, 0U, 0U});
        ASSERT_TRUE(original.has_value());
        shared::Block const replacement = *original == shared::Block::Snow ? shared::Block::Air : shared::Block::Snow;
        ASSERT_TRUE(chunk->setBlock({0U, 0U, 0U}, replacement));
        expected_hash = (expected_hash ^ section) * 1'099'511'628'211ULL;
        expected_hash = (expected_hash ^ chunk->contentIdentity().content_hash) * 1'099'511'628'211ULL;
        auto const changed = world.currentAuthoritativeTileVersionState(TILE);
        ASSERT_TRUE(changed.has_value());
        EXPECT_EQ(changed->dense_content_hash, expected_hash);
        EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
            TILE, identity, baseline->revision, baseline->dense_content_hash));
    }
    auto const changed_column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(changed_column.has_value());
    EXPECT_EQ(changed_column->dense_section_mask.count(), 4U);
    for (uint32_t const section : {0U, 63U, 64U, 127U}) {
        shared::ChunkCoordinate const key{4, 5, static_cast<int32_t>(section)};
        auto* const chunk = world.ensureChunk(key);
        ASSERT_NE(chunk, nullptr);
        auto const canonical = makeCanonicalChunk(surface, key);
        ASSERT_TRUE(chunk->setBlock({0U, 0U, 0U}, *canonical.blockAt({0U, 0U, 0U})));
        ASSERT_TRUE(world.evict(key));
    }
    auto const restored = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->dense_content_hash, baseline->dense_content_hash);
    EXPECT_TRUE(world.matchesAuthoritativeTileVersion(
        TILE, identity, baseline->revision, baseline->dense_content_hash));
    auto const restored_column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(restored_column.has_value());
    EXPECT_TRUE(restored_column->dense_section_mask.none());
}

TEST(SparseWorldTest, HighAuthoritativeProfileRetainsTopWorldCellAndExactUpperEdit)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::AuthoritativeTerrainTile tile{.coordinate = {4, 5, 0}, .identity = identity};
    tile.columns.fill({.highest_solid_z = 2'047U, .ground_first_z = 2'047U, .ground_end_z = 2'048U,
        .top_material = shared::Block::Snow, .ground_material = shared::Block::Stone});
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(tile));
    auto const base = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base->highest_solid_z, 2'047U);
    EXPECT_TRUE(base->dense_section_mask.none());
    EXPECT_EQ(base->materialAt(2'047U), shared::Block::Snow);
    EXPECT_EQ(world.queryBlock({64, 80, 2'047}).block, shared::Block::Snow);
    EXPECT_FALSE(world.ensureChunk({4, 5, 128}));
    ASSERT_TRUE(world.setBlock({64, 80, 2'047}, shared::Block::Air));
    auto const edited = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(edited.has_value());
    EXPECT_EQ(edited->dense_section_mask, shared::DenseSectionMask{1U} << 127U);
    EXPECT_EQ(edited->highest_solid_z, 2'046U);
    EXPECT_EQ(edited->materialAt(2'047U), shared::Block::Air);
    ASSERT_TRUE(world.evict({4, 5, 127}));
    EXPECT_EQ(world.queryBlock({64, 80, 2'047}).block, shared::Block::Air);
    EXPECT_FALSE(world.setBlock({64, 80, 2'048}, shared::Block::Snow));
}

TEST(SparseWorldTest, AuthoritativePublicationRejectsInvalidIdentityAndProtectsColumnRetention)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U,
        .max_authoritative_columns = 1U}, shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    std::unique_ptr<shared::SurfaceTile> const surface_owner{
        new shared::SurfaceTile(makeCanonicalSurface({.x = 4, .y = 5, .z = 0}, identity))
    };
    shared::SurfaceTile const& surface = *surface_owner;
    std::unique_ptr<shared::SurfaceTile> const invalid_owner{ new shared::SurfaceTile(surface) };
    shared::SurfaceTile& invalid = *invalid_owner;
    invalid.identity.seed = 43U;
    EXPECT_FALSE(world.publishAuthoritativeSurface(invalid));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    std::array const protected_columns{shared::ChunkCoordinate{4, 5, 0}};
    ASSERT_TRUE(world.setProtectedAuthoritativeColumns(protected_columns));
    std::unique_ptr<shared::SurfaceTile> const other_owner{
        new shared::SurfaceTile(makeCanonicalSurface({.x = 6, .y = 5, .z = 0}, identity))
    };
    shared::SurfaceTile const& other = *other_owner;
    EXPECT_FALSE(world.publishAuthoritativeSurface(other));
    EXPECT_FALSE(world.removeAuthoritativeSurface({4, 5, 0}));
    ASSERT_TRUE(world.setProtectedAuthoritativeColumns({}));
    ASSERT_TRUE(world.publishAuthoritativeSurface(other));
    EXPECT_FALSE(world.hasAuthoritativeChunk({4, 5, 0}));
    EXPECT_TRUE(world.hasAuthoritativeChunk({6, 5, 63}));
    EXPECT_EQ(world.availableAuthoritativeColumns().size(), 1U);
}

TEST(SparseWorldTest, AuthorityEvictionPreservesProtectedAndEditedColumnsAcrossProtectionChanges)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U,
        .max_authoritative_columns = 3U}, shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    for (int32_t x = 0; x < 3; ++x) {
        ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({x, 0, 0}, identity)));
    }
    std::array const first_protection{shared::ChunkCoordinate{0, 0, 0}};
    ASSERT_TRUE(world.setProtectedAuthoritativeColumns(first_protection));
    auto const original = world.queryBlock({16, 0, 900}).block;
    ASSERT_TRUE(original.has_value());
    ASSERT_TRUE(world.setBlock({16, 0, 900}, shared::Block::Snow));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({3, 0, 0}, identity)));
    EXPECT_TRUE(world.hasAuthoritativeSurface({0, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({1, 0, 0}));
    EXPECT_FALSE(world.hasAuthoritativeSurface({2, 0, 0}));
    EXPECT_EQ(world.queryBlock({16, 0, 900}).block, shared::Block::Snow);

    ASSERT_TRUE(world.setBlock({16, 0, 900}, *original));
    EXPECT_EQ(world.editChunkRevision({1, 0, 56}), 2U);
    EXPECT_EQ(world.cellVersion({16, 0, 900}), 2U);
    EXPECT_FALSE(world.removeAuthoritativeSurface({1, 0, 0}));
    ASSERT_TRUE(world.evict({1, 0, 56}));
    EXPECT_EQ(world.residentChunk({1, 0, 56}), nullptr);
    EXPECT_TRUE(world.hasAuthoritativeChunk({1, 0, 56}));
    EXPECT_EQ(world.queryBlock({16, 0, 900}).block, original);
    auto const edited_column = world.authoritativeColumn(16, 0);
    ASSERT_TRUE(edited_column.has_value());
    if (!edited_column) {
        return;
    }
    EXPECT_EQ(edited_column->dense_section_mask, shared::DenseSectionMask{1U} << 56U);
    ASSERT_NE(edited_column->dense_blocks, nullptr);
    EXPECT_EQ(edited_column->materialAt(900U), *original);
    auto* const rematerialized = world.ensureChunk({1, 0, 56});
    ASSERT_NE(rematerialized, nullptr);
    EXPECT_EQ(rematerialized->blockAt({0U, 0U, 4U}), original);
    EXPECT_EQ(world.editChunkRevision({1, 0, 56}), 2U);
    EXPECT_EQ(world.cellVersion({16, 0, 900}), 2U);
    std::array const next_protection{shared::ChunkCoordinate{3, 0, 0}};
    ASSERT_TRUE(world.setProtectedAuthoritativeColumns(next_protection));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({4, 0, 0}, identity)));
    EXPECT_FALSE(world.hasAuthoritativeSurface({0, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({1, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({3, 0, 0}));
    EXPECT_EQ(world.availableAuthoritativeColumns().size(), 3U);
}

TEST(SparseWorldTest, AuthorityEvictionIndexSurvivesExplicitRemovalAndIdentityReset)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U,
        .max_authoritative_columns = 2U}, shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({0, 0, 0}, identity)));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({1, 0, 0}, identity)));
    ASSERT_TRUE(world.removeAuthoritativeSurface({0, 0, 0}));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({2, 0, 0}, identity)));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({3, 0, 0}, identity)));
    EXPECT_FALSE(world.hasAuthoritativeSurface({1, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({2, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({3, 0, 0}));
    std::array const protection{shared::ChunkCoordinate{2, 0, 0}, shared::ChunkCoordinate{3, 0, 0}};
    ASSERT_TRUE(world.setProtectedAuthoritativeColumns(protection));
    EXPECT_FALSE(world.publishAuthoritativeSurface(makeCanonicalSurface({4, 0, 0}, identity)));
    world.setWorldIdentity(2U, 42U);
    auto const next_identity = testsupport::makeTerrainIdentity(42U, 2U);
    ASSERT_TRUE(world.setTerrainIdentity(next_identity));
    for (int32_t x = 4; x < 7; ++x) {
        ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({x, 0, 0}, next_identity)));
    }
    EXPECT_FALSE(world.hasAuthoritativeSurface({4, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({5, 0, 0}));
    EXPECT_TRUE(world.hasAuthoritativeSurface({6, 0, 0}));
    EXPECT_EQ(world.availableAuthoritativeColumns().size(), 2U);
}

TEST(SparseWorldTest, CanonicalDeepOceanEmptyGroundIntervalPreservesAllVoxelMaterials)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface({.x = 2'022, .y = 2'047, .z = 0}, identity, 300U);
    ASSERT_TRUE(shared::isValidSurfaceTile(surface));
    ASSERT_EQ(surface.columns[0].ground_first_z, surface.columns[0].ground_end_z);
    ASSERT_GT(surface.columns[0].ground_first_z, surface.columns[0].highest_solid_z + 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    for (uint16_t z = 0U; z < shared::WorldExtent::DEPTH; ++z) {
        EXPECT_EQ(world.queryBlock({32'352, 32'752, z}).block,
            surface.columns[0].materialAt(z, surface.block_sea_level));
    }
    for (int32_t z = 0; z < 64; ++z) {
        shared::ChunkCoordinate const key{2'022, 2'047, z};
        auto const* actual = world.ensureChunk(key);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(actual->contentIdentity().content_hash, makeCanonicalChunk(surface, key).contentIdentity().content_hash);
    }
    auto const authoritative_tile = world.authoritativeTile(surface.coordinate);
    ASSERT_TRUE(authoritative_tile.has_value());
    if (!authoritative_tile) {
        return;
    }
    auto invalid = *authoritative_tile;
    invalid.coordinate.x += 1;
    invalid.columns[0].ground_first_z -= 1U;
    EXPECT_FALSE(world.publishAuthoritativeSurface(invalid));
    invalid.columns[0].ground_first_z = shared::WorldExtent::DEPTH + 1U;
    invalid.columns[0].ground_end_z = invalid.columns[0].ground_first_z;
    EXPECT_FALSE(world.publishAuthoritativeSurface(invalid));
}

TEST(SparseWorldTest, AuthorityContentIdentityIgnoresCanonicalDenseCacheChurnAndDetectsEdits)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface({.x = 4, .y = 5, .z = 0}, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const original_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(original_identity.has_value());
    if (!original_identity) {
        return;
    }
    auto const& original = *original_identity;
    ASSERT_NE(world.ensureChunk({4, 5, 24}), nullptr);
    auto const admitted_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(admitted_identity.has_value());
    if (!admitted_identity) {
        return;
    }
    auto const& admitted = *admitted_identity;
    EXPECT_EQ(admitted.content_hash, original.content_hash);
    EXPECT_EQ(admitted.revision, original.revision);
    ASSERT_TRUE(world.evict({4, 5, 24}));
    auto const evicted_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(evicted_identity.has_value());
    if (!evicted_identity) {
        return;
    }
    EXPECT_EQ(evicted_identity->content_hash, original.content_hash);
    EXPECT_EQ(evicted_identity->revision, original.revision);
    ASSERT_NE(world.ensureChunk({4, 5, 24}), nullptr);
    ASSERT_TRUE(world.setBlock({64, 80, 385}, shared::Block::Snow));
    auto const edited_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(edited_identity.has_value());
    if (!edited_identity) {
        return;
    }
    EXPECT_NE(edited_identity->content_hash, original.content_hash);
    auto const& edited = *edited_identity;
    ASSERT_NE(world.ensureChunk({4, 5, 25}), nullptr);
    auto const admitted_upper_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(admitted_upper_identity.has_value());
    if (!admitted_upper_identity) {
        return;
    }
    EXPECT_EQ(admitted_upper_identity->content_hash, edited.content_hash);
    ASSERT_TRUE(world.evict({4, 5, 25}));
    auto const evicted_upper_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(evicted_upper_identity.has_value());
    if (!evicted_upper_identity) {
        return;
    }
    EXPECT_EQ(evicted_upper_identity->content_hash, edited.content_hash);
    EXPECT_EQ(evicted_upper_identity->revision, edited.revision);
}

TEST(SparseWorldTest, AuthorityContentIdentityDetectsDirectDenseEditsAndCertifiesEquivalentBaselines)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface({.x = 4, .y = 5, .z = 0}, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const initial_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(initial_identity.has_value());
    if (!initial_identity) {
        return;
    }
    auto const& initial = *initial_identity;
    auto* const chunk = world.ensureChunk({4, 5, 24});
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->setBlock({0, 0, 1}, shared::Block::Snow));
    auto const lower_edit_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(lower_edit_identity.has_value());
    if (!lower_edit_identity) {
        return;
    }
    EXPECT_NE(lower_edit_identity->content_hash, initial.content_hash);
    auto const lower_edit_column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(lower_edit_column.has_value());
    if (!lower_edit_column) {
        return;
    }
    EXPECT_EQ(lower_edit_column->dense_section_mask, shared::DenseSectionMask{1U} << 24U);

    auto* const upper = world.ensureChunk({4, 5, 25});
    ASSERT_NE(upper, nullptr);
    ASSERT_TRUE(upper->setBlock({0, 0, 1}, shared::Block::Snow));
    auto const upper_edit_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(upper_edit_identity.has_value());
    if (!upper_edit_identity) {
        return;
    }
    auto const upper_edit = upper_edit_identity->content_hash;
    ASSERT_TRUE(upper->setBlock({0, 0, 1}, shared::Block::Air));
    auto* const higher = world.ensureChunk({4, 5, 26});
    ASSERT_NE(higher, nullptr);
    ASSERT_TRUE(higher->setBlock({0, 0, 1}, shared::Block::Snow));
    auto const higher_edit_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(higher_edit_identity.has_value());
    if (!higher_edit_identity) {
        return;
    }
    EXPECT_NE(higher_edit_identity->content_hash, upper_edit);

    shared::SparseWorld unknown({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(unknown.setTerrainIdentity(identity));
    ASSERT_TRUE(unknown.publishMaterializedChunk(makeCanonicalChunk(surface, {4, 5, 24}), 1U, 42U));
    ASSERT_TRUE(unknown.publishAuthoritativeSurface(surface));
    auto const unknown_resident_identity = unknown.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(unknown_resident_identity.has_value());
    if (!unknown_resident_identity) {
        return;
    }
    auto const& unknown_resident = *unknown_resident_identity;
    EXPECT_EQ(unknown_resident.content_hash, initial.content_hash);
    auto const unknown_column = unknown.authoritativeColumn(64, 80);
    ASSERT_TRUE(unknown_column.has_value());
    if (!unknown_column) {
        return;
    }
    EXPECT_EQ(unknown_column->dense_section_mask, shared::DenseSectionMask{});
    EXPECT_EQ(unknown_column->dense_blocks, nullptr);
    EXPECT_EQ(unknown_column->highest_solid_z, surface.columns[0].highest_solid_z);
    for (uint16_t z = 0U; z < shared::WorldExtent::DEPTH; ++z) {
        auto const query = unknown.queryBlock({64, 80, z});
        EXPECT_EQ(query.state, shared::GenerationState::Materialized);
        EXPECT_EQ(query.block, surface.columns[0].materialAt(z, surface.block_sea_level));
    }
    ASSERT_TRUE(unknown.evict({4, 5, 24}));
    auto const unknown_evicted_identity = unknown.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(unknown_evicted_identity.has_value());
    if (!unknown_evicted_identity) {
        return;
    }
    EXPECT_EQ(unknown_evicted_identity->content_hash, unknown_resident.content_hash);
}

TEST(SparseWorldTest, ExactAuthorityIdentityMatchPreservesEverySnapshotFence)
{
    static constexpr shared::ChunkCoordinate COORDINATE{4, 5, 0};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(COORDINATE, identity);
    shared::SparseWorld world({.seed = 42U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(COORDINATE, identity, 1U, 0U));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const snapshot = world.authoritativeTileIdentity(COORDINATE);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    auto different = snapshot->identity;
    ++different.seed;
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, different, snapshot->revision, snapshot->content_hash
    ));
    different = snapshot->identity;
    ++different.revision;
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, different, snapshot->revision, snapshot->content_hash
    ));
    different = snapshot->identity;
    ++different.terrain_version;
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, different, snapshot->revision, snapshot->content_hash
    ));
    different = snapshot->identity;
    different.configuration_identity += ";different";
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, different, snapshot->revision, snapshot->content_hash
    ));
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision + 1U, snapshot->content_hash
    ));
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash ^ 1U
    ));
    ASSERT_TRUE(world.removeAuthoritativeSurface(COORDINATE));
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    EXPECT_EQ(snapshot->identity, identity);
}

TEST(SparseWorldTest, ExactAuthorityIdentityMatchDetectsDirectDenseEditWithoutGlobalRevisionChange)
{
    static constexpr shared::ChunkCoordinate COORDINATE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE_COORDINATE{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(COORDINATE, identity);
    shared::SparseWorld world({.seed = 42U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto* const chunk = world.ensureChunk(DENSE_COORDINATE);
    ASSERT_NE(chunk, nullptr);
    auto const snapshot = world.authoritativeTileIdentity(COORDINATE);
    ASSERT_TRUE(snapshot.has_value());
    uint64_t const revision = world.authoritativeSurfaceRevision();
    EXPECT_TRUE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    ASSERT_TRUE(chunk->setBlock({0, 0, 1}, shared::Block::Snow));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), revision);
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    auto const edited = world.authoritativeTileIdentity(COORDINATE);
    ASSERT_TRUE(edited.has_value());
    EXPECT_TRUE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, edited->identity, edited->revision, edited->content_hash
    ));
}

TEST(SparseWorldTest, RetiredAuthorityVersionDetectsDirectDenseEditWithoutGlobalRevisionChange)
{
    static constexpr shared::ChunkCoordinate COORDINATE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE_COORDINATE{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(COORDINATE, identity)));
    shared::Chunk* const dense = world.ensureChunk(DENSE_COORDINATE);
    ASSERT_NE(dense, nullptr);
    auto const captured = world.currentAuthoritativeTileVersionState(COORDINATE);
    ASSERT_TRUE(captured);
    std::array<shared::AuthoritativeTileVersion, 1> versions{{
        {.coordinate = COORDINATE, .identity = identity, .revision = captured->revision,
            .dense_content_hash = captured->dense_content_hash},
    }};
    ASSERT_TRUE(world.removeAuthoritativeSurface(COORDINATE));
    uint64_t const global_revision = world.authoritativeSurfaceRevision();
    auto const retired = world.currentAuthoritativeTileVersionState(COORDINATE);
    ASSERT_TRUE(retired);
    EXPECT_EQ(retired->revision, captured->revision);
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));
    auto const before = dense->blockAt({0U, 0U, 1U});
    ASSERT_TRUE(before);
    shared::Block const replacement = *before == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;

    ASSERT_TRUE(dense->setBlock({0U, 0U, 1U}, replacement));

    EXPECT_EQ(world.authoritativeSurfaceRevision(), global_revision);
    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(COORDINATE).has_value());
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));
}

TEST(SparseWorldTest, ExactAuthorityIdentityMatchDetectsDifferentDenseBaselineEviction)
{
    static constexpr shared::ChunkCoordinate COORDINATE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE_COORDINATE{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(COORDINATE, identity);
    shared::SparseWorld world({.seed = 42U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    auto dense = makeCanonicalChunk(surface, DENSE_COORDINATE);
    ASSERT_TRUE(dense.setBlock({0U, 0U, 1U}, shared::Block::Snow));
    ASSERT_TRUE(world.publishMaterializedChunk(dense, 1U, 42U));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const snapshot = world.authoritativeTileIdentity(COORDINATE);
    ASSERT_TRUE(snapshot.has_value());
    uint64_t const revision = world.authoritativeSurfaceRevision();
    EXPECT_TRUE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    ASSERT_TRUE(world.evict(DENSE_COORDINATE));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), revision);
    EXPECT_FALSE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, snapshot->identity, snapshot->revision, snapshot->content_hash
    ));
    auto const evicted = world.authoritativeTileIdentity(COORDINATE);
    ASSERT_TRUE(evicted.has_value());
    EXPECT_TRUE(world.matchesAuthoritativeTileIdentity(
        COORDINATE, evicted->identity, evicted->revision, evicted->content_hash
    ));
}

TEST(SparseWorldTest, SparseEditsSurviveEvictionAndWrappedRematerialization)
{
    static constexpr uint64_t WORLD_SEED = 42U;
    static constexpr uint64_t WORLD_REVISION = 1U;
    static constexpr shared::ChunkCoordinate TILE{1, 1, 0};
    static constexpr shared::ChunkCoordinate WRAPPED_TILE{4'095, 1, 0};
    struct ExpectedEdit final {
        shared::WorldCoordinate coordinate;
        shared::Block block;
    };
    static constexpr std::array<ExpectedEdit, 5U> EXPECTED_EDITS{
        ExpectedEdit{{16, 16, 384}, shared::Block::Air},
        ExpectedEdit{{16, 16, 383}, shared::Block::Air},
        ExpectedEdit{{16, 16, 700}, shared::Block::Stone},
        ExpectedEdit{{16, 16, 710}, shared::Block::Snow},
        ExpectedEdit{{-1, 16, 0}, shared::Block::Air},
    };

    auto const identity = testsupport::makeTerrainIdentity(WORLD_SEED, WORLD_REVISION);
    shared::SparseWorld world(
        {
            .seed = WORLD_SEED,
            .max_resident_chunks = 1U,
            .revision = WORLD_REVISION,
            .max_authoritative_columns = 2U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(WRAPPED_TILE, identity)));

    ASSERT_TRUE(world.setBlock({16, 16, 384}, shared::Block::Air));
    ASSERT_TRUE(world.setBlock({16, 16, 383}, shared::Block::Air));
    ASSERT_TRUE(world.setBlock({16, 16, 700}, shared::Block::Stone));
    ASSERT_TRUE(world.setBlock({16, 16, 701}, shared::Block::Stone));
    ASSERT_TRUE(world.setBlock({16, 16, 701}, shared::Block::Air));
    ASSERT_TRUE(world.setBlock({16, 16, 710}, shared::Block::Snow));
    ASSERT_TRUE(world.setBlock({-1, 16, 0}, shared::Block::Air));
    EXPECT_LE(world.residentChunkCount(), world.maxResidentChunks());

    for (ExpectedEdit const& edit : EXPECTED_EDITS) {
        auto const normalized = shared::WorldBounds::normalize(edit.coordinate);
        ASSERT_TRUE(normalized.has_value());
        auto* const chunk = world.ensureChunk(shared::WorldBounds::chunkCoordinate(*normalized));
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(chunk->blockAt(shared::WorldBounds::blockCoordinate(*normalized)), edit.block);
        EXPECT_EQ(world.queryBlock(edit.coordinate).block, edit.block);
        EXPECT_LE(world.residentChunkCount(), world.maxResidentChunks());
    }
    auto const reset_column = world.authoritativeColumn(16, 16);
    ASSERT_TRUE(reset_column.has_value());
    EXPECT_EQ(reset_column->materialAt(700U), shared::Block::Stone);
    EXPECT_EQ(reset_column->materialAt(701U), shared::Block::Air);
    EXPECT_EQ(reset_column->materialAt(710U), shared::Block::Snow);
    EXPECT_EQ(reset_column->highest_solid_z, 710U);
}

TEST(SparseWorldTest, BaseEqualResetSurvivesDenseEvictionAndRematerialization)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 43};
    static constexpr shared::WorldCoordinate RESET_BLOCK{64, 80, 701};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 1U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));

    ASSERT_TRUE(world.setBlock(RESET_BLOCK, shared::Block::Snow));
    ASSERT_TRUE(world.setBlock(RESET_BLOCK, shared::Block::Air));
    auto const reset_identity = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(reset_identity.has_value());
    ASSERT_TRUE(world.evict(EDITED_CHUNK));

    auto const evicted_identity = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(evicted_identity.has_value());
    EXPECT_EQ(evicted_identity->revision, reset_identity->revision);
    EXPECT_EQ(evicted_identity->content_hash, reset_identity->content_hash);
    auto const column = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(column.has_value());
    EXPECT_EQ(column->dense_section_mask, shared::DenseSectionMask{1U} << 43U);
    EXPECT_EQ(column->materialAt(701U), shared::Block::Air);

    auto* const chunk = world.ensureChunk(EDITED_CHUNK);
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->blockAt({0U, 0U, 13U}), shared::Block::Air);
}

TEST(SparseWorldTest, SameValueWritesDoNotAdvanceEditRevisions)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 385};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Snow));
    auto const after_change = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(after_change.has_value());
    auto const* const chunk = world.residentChunk(EDITED_CHUNK);
    ASSERT_NE(chunk, nullptr);
    uint64_t const chunk_revision = chunk->contentIdentity().revision;
    uint64_t const edit_revision = world.editChunkRevision(EDITED_CHUNK);
    uint64_t const cell_version = world.cellVersion(EDITED_BLOCK);
    uint64_t const surface_revision = world.authoritativeSurfaceRevision();

    EXPECT_FALSE(world.setBlock(EDITED_BLOCK, shared::Block::Snow));

    auto const after_no_op = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(after_no_op.has_value());
    EXPECT_EQ(after_no_op->revision, after_change->revision);
    EXPECT_EQ(after_no_op->content_hash, after_change->content_hash);
    EXPECT_EQ(world.residentChunk(EDITED_CHUNK)->contentIdentity().revision, chunk_revision);
    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), edit_revision);
    EXPECT_EQ(world.cellVersion(EDITED_BLOCK), cell_version);
    EXPECT_EQ(world.authoritativeSurfaceRevision(), surface_revision);
}

TEST(SparseWorldTest, SparseEditRevisionTracksManagedEditsAndIdentityClears)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 385};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    uint64_t const initial_revision = world.sparseEditRevision();

    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Snow));
    uint64_t const after_set = world.sparseEditRevision();
    EXPECT_GT(after_set, initial_revision);
    EXPECT_FALSE(world.setBlock(EDITED_BLOCK, shared::Block::Snow));
    EXPECT_EQ(world.sparseEditRevision(), after_set);
    ASSERT_TRUE(world.applyEditDelta(EDITED_BLOCK, shared::Block::Ice, 1U, 2U, 2U));
    uint64_t const after_delta = world.sparseEditRevision();
    EXPECT_GT(after_delta, after_set);

    auto snapshot = world.editSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->edits.size(), 1U);
    auto effective_chunk = world.effectiveChunkSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(effective_chunk.has_value());
    snapshot->revision = 3U;
    snapshot->edits[0].material = shared::Block::Grass;
    snapshot->edits[0].cell_version = 3U;
    ASSERT_TRUE(effective_chunk->setBlock({0U, 0U, 1U}, shared::Block::Grass));
    ASSERT_TRUE(world.installEditSnapshot(*snapshot, *effective_chunk));
    uint64_t const after_snapshot = world.sparseEditRevision();
    EXPECT_GT(after_snapshot, after_delta);

    uint64_t const before_direct_mutation = world.sparseEditRevision();
    shared::Chunk* const mutable_chunk = world.ensureChunk(EDITED_CHUNK);
    ASSERT_NE(mutable_chunk, nullptr);
    auto const previous = mutable_chunk->blockAt({1U, 0U, 1U});
    ASSERT_TRUE(previous.has_value());
    shared::Block const replacement = *previous == shared::Block::Stone
        ? shared::Block::Air
        : shared::Block::Stone;
    ASSERT_TRUE(mutable_chunk->setBlock({1U, 0U, 1U}, replacement));
    EXPECT_EQ(world.sparseEditRevision(), before_direct_mutation);

    std::array<shared::EditedChunkRevision, 1> requirements{{
        {.coordinate = EDITED_CHUNK, .revision = 4U},
    }};
    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, requirements));
    EXPECT_EQ(world.sparseEditRevision(), before_direct_mutation);
    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, {}));
    EXPECT_EQ(world.sparseEditRevision(), before_direct_mutation);

    world.setWorldIdentity(2U, 42U);
    EXPECT_GT(world.sparseEditRevision(), before_direct_mutation);
}

TEST(SparseWorldTest, EditOverlayCapacityRejectsNewCellsAndAllowsRetainedCellWrites)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::ChunkCoordinate REJECTED_CHUNK{4, 5, 43};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 385};
    static constexpr shared::WorldCoordinate REJECTED_BLOCK{64, 80, 700};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world(
        {
            .seed = 42U,
            .max_resident_chunks = 2U,
            .revision = 1U,
            .max_retained_edit_cells = 1U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));

    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Snow));
    auto const original_identity = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(original_identity.has_value());
    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Air));
    EXPECT_FALSE(world.setBlock(EDITED_BLOCK, shared::Block::Air));

    EXPECT_FALSE(world.setBlock(REJECTED_BLOCK, shared::Block::Stone));

    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 2U);
    EXPECT_EQ(world.cellVersion(EDITED_BLOCK), 2U);
    EXPECT_EQ(world.queryBlock(EDITED_BLOCK).block, shared::Block::Air);
    EXPECT_EQ(world.editChunkRevision(REJECTED_CHUNK), 0U);
    EXPECT_EQ(world.cellVersion(REJECTED_BLOCK), 0U);
    EXPECT_FALSE(world.editSnapshot(REJECTED_CHUNK).has_value());
    EXPECT_EQ(world.residentChunk(REJECTED_CHUNK), nullptr);
    auto const after_rejection_identity = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(after_rejection_identity.has_value());
    EXPECT_EQ(after_rejection_identity->revision, original_identity->revision + 1U);
    EXPECT_NE(after_rejection_identity->content_hash, original_identity->content_hash);
}

TEST(SparseWorldTest, EditOverlayCapacityRejectsOversizedRemoteSnapshotAtomically)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {
            .seed = 42U,
            .max_resident_chunks = 2U,
            .revision = 1U,
            .max_retained_edit_cells = 1U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const original_identity = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(original_identity.has_value());
    auto effective_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
    ASSERT_TRUE(effective_chunk.setBlock({0U, 0U, 0U}, shared::Block::Snow));
    ASSERT_TRUE(effective_chunk.setBlock({1U, 0U, 0U}, shared::Block::Stone));
    shared::SparseEditSnapshot snapshot{
        .coordinate = EDITED_CHUNK,
        .revision = 2U,
        .edits = {
            {{0U, 0U, 0U}, shared::Block::Snow, 1U},
            {{1U, 0U, 0U}, shared::Block::Stone, 2U},
        },
    };

    EXPECT_FALSE(world.installEditSnapshot(snapshot, effective_chunk));

    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 0U);
    EXPECT_FALSE(world.editSnapshot(EDITED_CHUNK).has_value());
    EXPECT_EQ(world.residentChunk(EDITED_CHUNK), nullptr);
    auto const after_rejection_identity = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(after_rejection_identity.has_value());
    EXPECT_EQ(after_rejection_identity->revision, original_identity->revision);
    EXPECT_EQ(after_rejection_identity->content_hash, original_identity->content_hash);
}

TEST(SparseWorldTest, EditOverlayCapacityRejectsNewRemoteDeltaWithoutLosingRetainedEdit)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate FIRST_CHUNK{4, 5, 24};
    static constexpr shared::ChunkCoordinate SECOND_CHUNK{4, 5, 43};
    static constexpr shared::WorldCoordinate FIRST_BLOCK{64, 80, 385};
    static constexpr shared::WorldCoordinate SECOND_BLOCK{64, 80, 700};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world(
        {
            .seed = 42U,
            .max_resident_chunks = 2U,
            .revision = 1U,
            .max_retained_edit_cells = 1U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    ASSERT_TRUE(world.applyEditDelta(FIRST_BLOCK, shared::Block::Snow, 0U, 1U, 1U));

    EXPECT_FALSE(world.applyEditDelta(SECOND_BLOCK, shared::Block::Stone, 0U, 1U, 1U));

    EXPECT_EQ(world.editChunkRevision(FIRST_CHUNK), 1U);
    EXPECT_EQ(world.cellVersion(FIRST_BLOCK), 1U);
    EXPECT_EQ(world.queryBlock(FIRST_BLOCK).block, shared::Block::Snow);
    EXPECT_EQ(world.editChunkRevision(SECOND_CHUNK), 0U);
    EXPECT_EQ(world.cellVersion(SECOND_BLOCK), 0U);
    EXPECT_FALSE(world.editSnapshot(SECOND_CHUNK).has_value());
}

TEST(SparseWorldTest, RepeatedRemoteSnapshotRotationCannotExceedStoredCellCapacity)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr uint64_t MAX_RETAINED_CELLS = 4U;
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {
            .seed = 42U,
            .max_resident_chunks = 2U,
            .revision = 1U,
            .max_retained_edit_cells = MAX_RETAINED_CELLS,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));

    auto install_single_cell_snapshot = [&](uint8_t const local_x, uint64_t const revision) {
        shared::BlockCoordinate const cell{local_x, 0U, 0U};
        auto effective_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
        if (!effective_chunk.setBlock(cell, shared::Block::Snow)) {
            return false;
        }
        shared::SparseEditSnapshot const snapshot{
            .coordinate = EDITED_CHUNK,
            .revision = revision,
            .edits = {{cell, shared::Block::Snow, revision}},
        };
        return world.installEditSnapshot(snapshot, effective_chunk);
    };

    ASSERT_TRUE(install_single_cell_snapshot(0U, 1U));
    ASSERT_TRUE(install_single_cell_snapshot(1U, 2U));
    ASSERT_TRUE(install_single_cell_snapshot(2U, 3U));
    ASSERT_TRUE(install_single_cell_snapshot(3U, 4U));
    auto const before_rejection = world.editSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(before_rejection.has_value());
    ASSERT_EQ(before_rejection->revision, 4U);
    auto const effective_before_rejection = world.effectiveChunkSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(effective_before_rejection.has_value());

    EXPECT_FALSE(install_single_cell_snapshot(4U, 5U));

    auto const after_rejection = world.editSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(after_rejection.has_value());
    EXPECT_EQ(after_rejection->revision, before_rejection->revision);
    ASSERT_EQ(after_rejection->edits.size(), 1U);
    EXPECT_EQ(after_rejection->edits[0].coordinate, (shared::BlockCoordinate{3U, 0U, 0U}));
    EXPECT_EQ(world.cellVersion({68, 80, 384}), 0U);
    auto const effective_after_rejection = world.effectiveChunkSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(effective_after_rejection.has_value());
    EXPECT_EQ(effective_after_rejection->blockAt({3U, 0U, 0U}), shared::Block::Snow);
    EXPECT_EQ(
        effective_after_rejection->blockAt({4U, 0U, 0U}),
        effective_before_rejection->blockAt({4U, 0U, 0U})
    );
}

TEST(SparseWorldTest, RetainedSnapshotTombstonesCanBeRevivedAtCapacity)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate RETAINED_BLOCK{64, 80, 384};
    static constexpr shared::WorldCoordinate NEW_BLOCK{65, 80, 384};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    auto make_world = [&]() {
        shared::SparseWorld world(
            {
                .seed = 42U,
                .max_resident_chunks = 2U,
                .revision = 1U,
                .max_retained_edit_cells = 1U,
            },
            shared::SparseWorldGenerationMode::MaterializedOnly
        );
        EXPECT_TRUE(world.setTerrainIdentity(identity));
        EXPECT_TRUE(world.publishAuthoritativeSurface(surface));
        auto effective_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
        EXPECT_TRUE(effective_chunk.setBlock({0U, 0U, 0U}, shared::Block::Snow));
        shared::SparseEditSnapshot const initial_snapshot{
            .coordinate = EDITED_CHUNK,
            .revision = 1U,
            .edits = {{{0U, 0U, 0U}, shared::Block::Snow, 1U}},
        };
        EXPECT_TRUE(world.installEditSnapshot(initial_snapshot, effective_chunk));
        auto const base_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
        shared::SparseEditSnapshot const cleared_snapshot{
            .coordinate = EDITED_CHUNK,
            .revision = 2U,
            .edits = {},
        };
        EXPECT_TRUE(world.installEditSnapshot(cleared_snapshot, base_chunk));
        return world;
    };

    {
        auto world = make_world();
        ASSERT_EQ(world.editChunkRevision(EDITED_CHUNK), 2U);
        ASSERT_EQ(world.cellVersion(RETAINED_BLOCK), 2U);

        EXPECT_TRUE(world.setBlock(RETAINED_BLOCK, shared::Block::Snow));

        EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 3U);
        EXPECT_EQ(world.cellVersion(RETAINED_BLOCK), 3U);
        EXPECT_FALSE(world.setBlock(NEW_BLOCK, shared::Block::Stone));
        EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 3U);
        EXPECT_EQ(world.cellVersion(NEW_BLOCK), 0U);
    }

    {
        auto world = make_world();

        EXPECT_TRUE(world.applyEditDelta(RETAINED_BLOCK, shared::Block::Snow, 2U, 3U, 3U));

        EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 3U);
        EXPECT_EQ(world.cellVersion(RETAINED_BLOCK), 3U);
        EXPECT_FALSE(world.applyEditDelta(NEW_BLOCK, shared::Block::Stone, 3U, 4U, 4U));
        EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 3U);
        EXPECT_EQ(world.cellVersion(NEW_BLOCK), 0U);
    }
}

TEST(SparseWorldTest, EmptyRevisionSnapshotsUseBoundedCapacityAndPreserveDeltaFences)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate FIRST_CHUNK{4, 5, 24};
    static constexpr shared::ChunkCoordinate SECOND_CHUNK{4, 5, 25};
    static constexpr shared::ChunkCoordinate REJECTED_CHUNK{4, 5, 26};
    static constexpr shared::WorldCoordinate FIRST_BLOCK{64, 80, 384};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {
            .seed = 42U,
            .max_resident_chunks = 2U,
            .revision = 1U,
            .max_retained_edit_cells = 2U,
        },
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));

    for (shared::ChunkCoordinate const coordinate : {FIRST_CHUNK, SECOND_CHUNK}) {
        auto const effective_chunk = makeCanonicalChunk(surface, coordinate);
        shared::SparseEditSnapshot const snapshot{
            .coordinate = coordinate,
            .revision = 1U,
            .edits = {},
        };
        ASSERT_TRUE(world.installEditSnapshot(snapshot, effective_chunk));
    }
    EXPECT_EQ(world.residentChunkCount(), 2U);

    auto const rejected_chunk = makeCanonicalChunk(surface, REJECTED_CHUNK);
    shared::SparseEditSnapshot const rejected_snapshot{
        .coordinate = REJECTED_CHUNK,
        .revision = 1U,
        .edits = {},
    };
    EXPECT_FALSE(world.installEditSnapshot(rejected_snapshot, rejected_chunk));

    EXPECT_EQ(world.editChunkRevision(FIRST_CHUNK), 1U);
    EXPECT_TRUE(world.editSnapshot(FIRST_CHUNK).has_value());
    EXPECT_EQ(world.editChunkRevision(SECOND_CHUNK), 1U);
    EXPECT_TRUE(world.editSnapshot(SECOND_CHUNK).has_value());
    EXPECT_EQ(world.editChunkRevision(REJECTED_CHUNK), 0U);
    EXPECT_FALSE(world.editSnapshot(REJECTED_CHUNK).has_value());
    EXPECT_EQ(world.residentChunk(REJECTED_CHUNK), nullptr);
    EXPECT_EQ(world.residentChunkCount(), 2U);

    EXPECT_TRUE(world.applyEditDelta(FIRST_BLOCK, shared::Block::Snow, 1U, 2U, 2U));

    EXPECT_EQ(world.editChunkRevision(FIRST_CHUNK), 2U);
    EXPECT_EQ(world.cellVersion(FIRST_BLOCK), 2U);
    EXPECT_EQ(world.residentChunkCount(), 2U);
}

TEST(SparseWorldTest, SparseEditLeavesNeighboringAuthorityProfileSparse)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 900};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Stone));

    auto const edited = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(edited.has_value());
    EXPECT_EQ(edited->highest_solid_z, 900U);
    ASSERT_NE(edited->dense_blocks, nullptr);
    auto const untouched = world.authoritativeColumn(65, 80);
    ASSERT_TRUE(untouched.has_value());
    EXPECT_EQ(untouched->dense_section_mask, shared::DenseSectionMask{});
    EXPECT_EQ(untouched->dense_blocks, nullptr);
}

TEST(SparseWorldTest, EditOverlaySnapshotSurvivesEvictionWithCellVersions)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 43};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 700};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 1U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));

    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Stone));
    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, shared::Block::Air));
    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 2U);
    EXPECT_EQ(world.cellVersion(EDITED_BLOCK), 2U);
    auto const snapshot = world.editSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->revision, 2U);
    ASSERT_EQ(snapshot->edits.size(), 1U);
    EXPECT_EQ(snapshot->edits[0].coordinate, (shared::BlockCoordinate{0U, 0U, 12U}));
    EXPECT_EQ(snapshot->edits[0].material, shared::Block::Air);
    EXPECT_EQ(snapshot->edits[0].cell_version, 2U);

    auto const edited_chunks = world.editedChunks();
    ASSERT_EQ(edited_chunks.size(), 1U);
    EXPECT_EQ(edited_chunks[0].coordinate, EDITED_CHUNK);
    EXPECT_EQ(edited_chunks[0].revision, 2U);
    ASSERT_TRUE(world.evict(EDITED_CHUNK));
    EXPECT_EQ(world.blockAt(EDITED_BLOCK), shared::Block::Air);
    auto const effective_snapshot = world.effectiveChunkSnapshot(EDITED_CHUNK);
    ASSERT_TRUE(effective_snapshot.has_value());
    EXPECT_EQ(effective_snapshot->blockAt({0U, 0U, 12U}), shared::Block::Air);
    auto* const rematerialized = world.ensureChunk(EDITED_CHUNK);
    ASSERT_NE(rematerialized, nullptr);
    EXPECT_EQ(rematerialized->blockAt({0U, 0U, 12U}), shared::Block::Air);
}

TEST(SparseWorldTest, RemoteEditSnapshotsAndDeltasRejectStaleState)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 384};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto effective_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
    ASSERT_TRUE(effective_chunk.setBlock({0U, 0U, 0U}, shared::Block::Snow));
    shared::SparseEditSnapshot snapshot{
        .coordinate = EDITED_CHUNK,
        .revision = 2U,
        .edits = {{{0U, 0U, 0U}, shared::Block::Snow, 2U}},
    };

    ASSERT_TRUE(world.installEditSnapshot(snapshot, effective_chunk));
    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 2U);
    EXPECT_EQ(world.cellVersion(EDITED_BLOCK), 2U);
    EXPECT_EQ(world.queryBlock(EDITED_BLOCK).block, shared::Block::Snow);

    shared::SparseEditSnapshot stale_snapshot = snapshot;
    stale_snapshot.edits[0].material = shared::Block::Air;
    auto stale_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
    EXPECT_FALSE(world.installEditSnapshot(stale_snapshot, stale_chunk));
    EXPECT_FALSE(world.applyEditDelta(EDITED_BLOCK, shared::Block::Air, 1U, 2U, 3U));
    EXPECT_EQ(world.queryBlock(EDITED_BLOCK).block, shared::Block::Snow);
    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 2U);

    EXPECT_TRUE(world.applyEditDelta(EDITED_BLOCK, shared::Block::Air, 2U, 3U, 3U));
    EXPECT_EQ(world.queryBlock(EDITED_BLOCK).block, shared::Block::Air);
    EXPECT_EQ(world.cellVersion(EDITED_BLOCK), 3U);
    EXPECT_EQ(world.editChunkRevision(EDITED_CHUNK), 3U);
    EXPECT_FALSE(world.applyEditDelta(EDITED_BLOCK, shared::Block::Snow, 2U, 3U, 4U));
}

TEST(SparseWorldTest, EditedChunkManifestFencesProfileAuthorityUntilSnapshotArrives)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{64, 80, 384};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world(
        {.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly
    );
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    ASSERT_TRUE(world.publishSurfaceMetadata(makeCanonicalSurface(TILE, identity)));
    std::array<shared::EditedChunkRevision, 1U> requirements{{{EDITED_CHUNK, 2U}}};

    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, requirements));
    EXPECT_FALSE(world.hasAuthoritativeSurface(TILE));
    EXPECT_EQ(world.queryBlock(EDITED_BLOCK).state, shared::GenerationState::Unknown);
    EXPECT_EQ(world.queryBlock({64, 80, 900}).state, shared::GenerationState::Unknown);
    EXPECT_FALSE(world.authoritativeColumn(64, 80).has_value());
    EXPECT_FALSE(world.verifiedSurfaceColumn(64, 80).has_value());

    auto effective_chunk = makeCanonicalChunk(surface, EDITED_CHUNK);
    ASSERT_TRUE(effective_chunk.setBlock({0U, 0U, 0U}, shared::Block::Snow));
    shared::SparseEditSnapshot snapshot{
        .coordinate = EDITED_CHUNK,
        .revision = 2U,
        .edits = {{{0U, 0U, 0U}, shared::Block::Snow, 2U}},
    };
    ASSERT_TRUE(world.installEditSnapshot(snapshot, effective_chunk));

    EXPECT_TRUE(world.hasAuthoritativeSurface(TILE));
    EXPECT_EQ(world.queryBlock(EDITED_BLOCK).block, shared::Block::Snow);
    EXPECT_TRUE(world.authoritativeColumn(64, 80).has_value());
}

TEST(SparseWorldTest, AuthoritativeTileVersionTracksDenseMutationAndIgnoresCanonicalCacheChurn)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE_CHUNK{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const captured = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(captured.has_value());
    uint64_t const global_revision = world.authoritativeSurfaceRevision();
    auto const matches_capture = [&] {
        return world.matchesAuthoritativeTileVersion(
            {TILE.x, TILE.y, 24}, captured->identity, captured->revision, captured->dense_content_hash);
    };
    EXPECT_TRUE(matches_capture());
    auto different_identity = captured->identity;
    ++different_identity.seed;
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, different_identity, captured->revision, captured->dense_content_hash));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, captured->identity, captured->revision + 1U, captured->dense_content_hash));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, captured->identity, captured->revision, captured->dense_content_hash ^ 1U));

    ASSERT_NE(world.ensureChunk(DENSE_CHUNK), nullptr);
    auto const materialized = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(materialized.has_value());
    EXPECT_EQ(materialized->revision, captured->revision);
    EXPECT_EQ(materialized->dense_content_hash, captured->dense_content_hash);
    EXPECT_EQ(world.authoritativeSurfaceRevision(), global_revision);
    EXPECT_TRUE(matches_capture());
    ASSERT_TRUE(world.evict(DENSE_CHUNK));
    EXPECT_TRUE(matches_capture());

    auto* const dense_chunk = world.ensureChunk(DENSE_CHUNK);
    ASSERT_NE(dense_chunk, nullptr);
    auto const dense_previous = dense_chunk->blockAt({0U, 0U, 1U});
    ASSERT_TRUE(dense_previous.has_value());
    shared::Block const dense_replacement = *dense_previous == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;
    ASSERT_TRUE(dense_chunk->setBlock({0U, 0U, 1U}, dense_replacement));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), global_revision);
    EXPECT_FALSE(matches_capture());
    auto const changed = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(changed.has_value());
    EXPECT_EQ(changed->revision, captured->revision);
    EXPECT_NE(changed->dense_content_hash, captured->dense_content_hash);
    EXPECT_TRUE(world.matchesAuthoritativeTileVersion(
        TILE, changed->identity, changed->revision, changed->dense_content_hash));

    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));
    EXPECT_FALSE(matches_capture());
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const republished = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(republished.has_value());
    EXPECT_NE(republished->revision, captured->revision);
    EXPECT_EQ(republished->dense_content_hash, changed->dense_content_hash);
    EXPECT_FALSE(matches_capture());
    EXPECT_TRUE(world.matchesAuthoritativeTileVersion(
        TILE, republished->identity, republished->revision, republished->dense_content_hash));

    auto const previous_block = world.queryBlock({65, 80, 385}).block;
    ASSERT_TRUE(previous_block.has_value());
    shared::Block const edited_block = *previous_block == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;
    ASSERT_TRUE(world.setBlock({65, 80, 385}, edited_block));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, republished->identity, republished->revision, republished->dense_content_hash));
}

TEST(SparseWorldTest, DenseCacheEvictionPreservesEquivalentAuthorityInEitherPublicationOrder)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE{4, 5, 25};
    static constexpr shared::ChunkCoordinate PRESSURE{5, 5, 25};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    auto const dense = makeCanonicalChunk(surface, DENSE);

    for (bool const dense_first : {false, true}) {
        SCOPED_TRACE(dense_first);
        shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 1U, .revision = 1U},
            shared::SparseWorldGenerationMode::MaterializedOnly);
        ASSERT_TRUE(world.setTerrainIdentity(identity));
        if (dense_first) {
            ASSERT_TRUE(world.publishMaterializedChunk(dense, 1U, 42U));
        }
        ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
        if (!dense_first) {
            ASSERT_TRUE(world.publishMaterializedChunk(dense, 1U, 42U));
        }
        auto const captured = world.currentAuthoritativeTileVersionState(TILE);
        ASSERT_TRUE(captured);
        std::optional<shared::ChunkCoordinate> evicted;
        ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{PRESSURE}, 1U, 42U, &evicted));
        ASSERT_EQ(evicted, DENSE);
        EXPECT_EQ(world.queryBlock({64, 80, 400}).block, shared::Block::Air);
        EXPECT_TRUE(world.matchesAuthoritativeTileVersion(
            TILE, identity, captured->revision, captured->dense_content_hash));
    }
}

TEST(SparseWorldTest, DenseFirstAuthorityUsesCompactMaterialsWithSparseEdits)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE{4, 5, 25};
    static constexpr shared::WorldCoordinate EDITED{64, 80, 400};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    auto const dense = makeCanonicalChunk(surface, DENSE);

    for (bool const matches_edit : {true, false}) {
        SCOPED_TRACE(matches_edit);
        shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 1U, .revision = 1U},
            shared::SparseWorldGenerationMode::MaterializedOnly);
        ASSERT_TRUE(world.setTerrainIdentity(identity));
        ASSERT_TRUE(world.publishMaterializedChunk(dense, 1U, 42U));
        ASSERT_TRUE(world.setBlock(EDITED, shared::Block::Snow));
        auto const edit_revision = world.editChunkRevision(DENSE);
        ASSERT_NE(edit_revision, 0U);
        if (!matches_edit) {
            // The dense block matches unedited compact terrain, but disagrees with the retained edit.
            auto* const resident = world.ensureChunk(DENSE);
            ASSERT_NE(resident, nullptr);
            ASSERT_TRUE(resident->setBlock({0U, 0U, 0U}, shared::Block::Air));
        }
        ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
        auto const captured = world.currentAuthoritativeTileVersionState(TILE);
        ASSERT_TRUE(captured);
        ASSERT_TRUE(world.evict(DENSE));
        auto const after = world.currentAuthoritativeTileVersionState(TILE);
        ASSERT_TRUE(after);
        EXPECT_EQ(after->revision, captured->revision);
        EXPECT_EQ(world.editChunkRevision(DENSE), edit_revision);
        EXPECT_EQ(world.queryBlock(EDITED).block, shared::Block::Snow);
        EXPECT_EQ(world.matchesAuthoritativeTileVersion(
            TILE, identity, captured->revision, captured->dense_content_hash), matches_edit);
    }
}

TEST(SparseWorldTest, EvictingDifferentDenseDataReceivedBeforeCompactInvalidatesAuthority)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE{4, 5, 25};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    auto dense = makeCanonicalChunk(surface, DENSE);
    ASSERT_TRUE(dense.setBlock({0U, 0U, 0U}, shared::Block::Snow));

    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 1U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishMaterializedChunk(dense, 1U, 42U));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    auto const profile = world.authoritativeColumn(64, 80);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->materialAt(400), shared::Block::Snow);
    ASSERT_TRUE(world.evict(DENSE));
    EXPECT_EQ(world.queryBlock({64, 80, 400}).block, shared::Block::Air);
    auto const after = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->revision, captured->revision);
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, identity, captured->revision, captured->dense_content_hash));
}

TEST(SparseWorldTest, AuthoritativeTileVersionBatchPreservesAvailabilityAndDenseMutationChecks)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE_CHUNK{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const captured = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(captured.has_value());
    std::array<shared::AuthoritativeTileVersion, 1> versions{{
        {.coordinate = TILE, .revision = captured->revision, .dense_content_hash = captured->dense_content_hash},
    }};

    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));
    auto different_identity = identity;
    ++different_identity.seed;
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(different_identity, versions));
    auto different_revision = versions;
    ++different_revision[0].revision;
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, different_revision));

    uint64_t const surface_revision = world.authoritativeSurfaceRevision();
    std::array<shared::EditedChunkRevision, 1> requirements{{
        {.coordinate = DENSE_CHUNK, .revision = 1U},
    }};
    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, requirements));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), surface_revision);
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));
    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, {}));
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));

    ASSERT_NE(world.ensureChunk(DENSE_CHUNK), nullptr);
    ASSERT_TRUE(world.evict(DENSE_CHUNK));
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));
    shared::Chunk* const dense_chunk = world.ensureChunk(DENSE_CHUNK);
    ASSERT_NE(dense_chunk, nullptr);
    auto const previous = dense_chunk->blockAt({0U, 0U, 1U});
    ASSERT_TRUE(previous.has_value());
    shared::Block const replacement = *previous == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;
    ASSERT_TRUE(dense_chunk->setBlock({0U, 0U, 1U}, replacement));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));

    auto const changed = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(changed.has_value());
    versions[0] = {
        .coordinate = TILE,
        .revision = changed->revision,
        .dense_content_hash = changed->dense_content_hash,
    };
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));
    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));
}

TEST(SparseWorldTest, CanonicalAuthorityRemovalRetainsVersionWithoutRestoringAvailability)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U,
        .max_authoritative_columns = 1U}, shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    std::array<shared::AuthoritativeTileVersion, 1> versions{{
        {.coordinate = TILE, .identity = identity, .revision = captured->revision,
            .dense_content_hash = captured->dense_content_hash},
    }};

    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));

    EXPECT_FALSE(world.hasAuthoritativeSurface(TILE));
    EXPECT_FALSE(world.authoritativeTileIdentity(TILE).has_value());
    EXPECT_FALSE(world.authoritativeColumn(64, 80).has_value());
    EXPECT_EQ(world.queryBlock({64, 80, 385}).state, shared::GenerationState::Unknown);
    EXPECT_TRUE(world.availableAuthoritativeColumns().empty());
    auto const retained = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->revision, captured->revision);
    EXPECT_EQ(retained->dense_content_hash, captured->dense_content_hash);
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));

    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity, 386U)));
    auto const republished = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(republished);
    EXPECT_NE(republished->revision, captured->revision);
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));
}

TEST(SparseWorldTest, RetiredAuthorityVersionCacheExpiresOldestAtConfiguredAuthorityBound)
{
    static constexpr shared::ChunkCoordinate FIRST{4, 5, 0};
    static constexpr shared::ChunkCoordinate SECOND{5, 5, 0};
    static constexpr shared::ChunkCoordinate THIRD{6, 5, 0};
    static constexpr shared::ChunkCoordinate FOURTH{7, 5, 0};
    static constexpr shared::ChunkCoordinate FIFTH{8, 5, 0};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U,
        .max_authoritative_columns = 2U}, shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    auto publish_and_capture = [&](shared::ChunkCoordinate coordinate) {
        EXPECT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(coordinate, identity)));
        return world.currentAuthoritativeTileVersionState(coordinate);
    };

    auto const first = publish_and_capture(FIRST);
    ASSERT_TRUE(first);
    auto const second = publish_and_capture(SECOND);
    ASSERT_TRUE(second);
    auto const third = publish_and_capture(THIRD);
    ASSERT_TRUE(third);
    auto const fourth = publish_and_capture(FOURTH);
    ASSERT_TRUE(fourth);
    auto const fifth = publish_and_capture(FIFTH);
    ASSERT_TRUE(fifth);
    std::array<shared::AuthoritativeTileVersion, 1> second_version{{
        {.coordinate = SECOND, .identity = identity, .revision = second->revision,
            .dense_content_hash = second->dense_content_hash},
    }};
    std::array<shared::AuthoritativeTileVersion, 1> third_version{{
        {.coordinate = THIRD, .identity = identity, .revision = third->revision,
            .dense_content_hash = third->dense_content_hash},
    }};

    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(FIRST).has_value());
    auto const retained_second = world.currentAuthoritativeTileVersionState(SECOND);
    ASSERT_TRUE(retained_second);
    EXPECT_EQ(retained_second->revision, second->revision);
    auto const retained_third = world.currentAuthoritativeTileVersionState(THIRD);
    ASSERT_TRUE(retained_third);
    EXPECT_EQ(retained_third->revision, third->revision);
    EXPECT_EQ(world.queryBlock({64, 80, 385}).state, shared::GenerationState::Unknown);
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, second_version));
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, third_version));
}

TEST(SparseWorldTest, RetiredAuthorityVersionIsInvalidatedByRealEdit)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE{4, 5, 24};
    static constexpr shared::WorldCoordinate EDITED{64, 80, 385};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    auto const dense = makeCanonicalChunk(surface, DENSE);

    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishMaterializedChunk(dense, 1U, 42U));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));
    auto const retired = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(retired);
    EXPECT_EQ(retired->revision, captured->revision);
    auto const before = world.queryBlock(EDITED).block;
    ASSERT_TRUE(before);

    shared::Block const replacement = *before == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;
    ASSERT_TRUE(world.setBlock(EDITED, replacement));

    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(TILE).has_value());
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, identity, captured->revision, captured->dense_content_hash));
    EXPECT_EQ(world.queryBlock(EDITED).block, replacement);
}

TEST(SparseWorldTest, RetiredAuthorityVersionIsInvalidatedByEditRequirements)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));
    auto const retired = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(retired);
    EXPECT_EQ(retired->revision, captured->revision);
    std::array<shared::EditedChunkRevision, 1> requirements{{
        {.coordinate = DENSE, .revision = 1U},
    }};

    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, requirements));
    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(TILE).has_value());
    ASSERT_TRUE(world.setEditedChunkRequirements(TILE, {}));

    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(TILE).has_value());
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, identity, captured->revision, captured->dense_content_hash));
}

TEST(SparseWorldTest, RetiredAuthorityVersionDoesNotSurviveNoncanonicalDenseData)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);

    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    shared::Chunk* const dense = world.ensureChunk(DENSE);
    ASSERT_NE(dense, nullptr);
    auto const previous = dense->blockAt({0U, 0U, 1U});
    ASSERT_TRUE(previous);
    shared::Block const replacement = *previous == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;
    ASSERT_TRUE(dense->setBlock({0U, 0U, 1U}, replacement));
    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));

    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(TILE).has_value());
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, identity, captured->revision, captured->dense_content_hash));
}

TEST(SparseWorldTest, RetiredAuthorityVersionCacheClearsOnWorldIdentityReset)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));
    auto const retired = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(retired);
    EXPECT_EQ(retired->revision, captured->revision);
    world.setWorldIdentity(2U, 43U);

    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(TILE).has_value());
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, identity, captured->revision, captured->dense_content_hash));
}

TEST(SparseWorldTest, RetiredAuthorityVersionCacheClearsOnTerrainIdentityReset)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface(TILE, identity)));
    auto const captured = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(captured);
    ASSERT_TRUE(world.removeAuthoritativeSurface(TILE));
    auto const retired = world.currentAuthoritativeTileVersionState(TILE);
    ASSERT_TRUE(retired);
    EXPECT_EQ(retired->revision, captured->revision);
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    shared::TerrainIdentity const next_identity{
        .terrain_version = generation::terrainVersion(generation::TerrainModelTag::V10),
        .revision = 1U,
        .seed = 42U,
        .configuration_identity = generation::configurationIdentity(configuration),
    };

    ASSERT_TRUE(world.setTerrainIdentity(next_identity));

    EXPECT_FALSE(world.currentAuthoritativeTileVersionState(TILE).has_value());
    EXPECT_FALSE(world.matchesAuthoritativeTileVersion(
        TILE, identity, captured->revision, captured->dense_content_hash));
}

TEST(SparseWorldTest, AuthoritativeTileVersionBatchPreservesSparseEditsAcrossEviction)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate EDITED_CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{65, 80, 385};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));

    auto const captured = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(captured.has_value());
    std::array<shared::AuthoritativeTileVersion, 1> versions{{
        {.coordinate = TILE, .revision = captured->revision, .dense_content_hash = captured->dense_content_hash},
    }};
    ASSERT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));

    auto const previous = world.queryBlock(EDITED_BLOCK).block;
    ASSERT_TRUE(previous.has_value());
    shared::Block const replacement = *previous == shared::Block::Snow
        ? shared::Block::Air
        : shared::Block::Snow;
    ASSERT_TRUE(world.setBlock(EDITED_BLOCK, replacement));
    EXPECT_FALSE(world.matchesAuthoritativeTileVersions(identity, versions));

    auto const edited = world.authoritativeTileIdentity(TILE);
    ASSERT_TRUE(edited.has_value());
    versions[0].revision = edited->revision;
    versions[0].dense_content_hash = edited->dense_content_hash;
    ASSERT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));
    ASSERT_TRUE(world.evict(EDITED_CHUNK));
    EXPECT_TRUE(world.matchesAuthoritativeTileVersions(identity, versions));
}

TEST(SparseWorldTest, ColumnJournalSupportsIndependentConsumersAndResidentEviction)
{
    shared::SparseWorld world({.max_resident_chunks = 1U});
    shared::AuthoritativeColumnChanges first;
    shared::AuthoritativeColumnChanges second;
    world.readAuthoritativeColumnChanges(0U, first);
    ASSERT_FALSE(first.full_resync);
    ASSERT_NE(world.ensureChunk({3, 2, 1}), nullptr);
    world.readAuthoritativeColumnChanges(0U, first);
    EXPECT_EQ(first.coordinates, (std::vector<shared::ChunkCoordinate>{{3, 2, 0}}));
    uint64_t const first_cursor = first.cursor;
    ASSERT_NE(world.ensureChunk({1, 2, 2}), nullptr);
    world.readAuthoritativeColumnChanges(first_cursor, first);
    world.readAuthoritativeColumnChanges(0U, second);
    EXPECT_EQ(first.coordinates, (std::vector<shared::ChunkCoordinate>{{1, 2, 0}, {3, 2, 0}}));
    EXPECT_EQ(second.coordinates, first.coordinates);
    ASSERT_TRUE(world.evict({1, 2, 2}));
    world.readAuthoritativeColumnChanges(first.cursor, first);
    EXPECT_EQ(first.coordinates, (std::vector<shared::ChunkCoordinate>{{1, 2, 0}}));
    world.readAuthoritativeColumnChanges(first.cursor, first);
    EXPECT_TRUE(first.coordinates.empty());
}

TEST(SparseWorldTest, ColumnJournalResynchronizesAfterResetLostHistoryAndInvalidCursor)
{
    shared::SparseWorld world({.max_resident_chunks = 1U});
    shared::AuthoritativeColumnChanges changes;
    for (uint32_t index = 0U; index < 2050U; ++index) {
        ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{{1, 2, 1}}, 1U, 42U));
        ASSERT_TRUE(world.evict({1, 2, 1}));
    }
    world.readAuthoritativeColumnChanges(0U, changes);
    EXPECT_TRUE(changes.full_resync);
    uint64_t const before_reset = changes.cursor;
    world.setWorldIdentity(2U, 42U);
    world.readAuthoritativeColumnChanges(before_reset, changes);
    EXPECT_TRUE(changes.full_resync);
    world.readAuthoritativeColumnChanges(changes.cursor, changes);
    EXPECT_FALSE(changes.full_resync);
    EXPECT_TRUE(changes.coordinates.empty());
    world.readAuthoritativeColumnChanges(changes.cursor + 1U, changes);
    EXPECT_TRUE(changes.full_resync);
}

TEST(SparseWorldTest, ColumnJournalTracksAuthorityPublicationVictimsRemovalAndRequirementChanges)
{
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    shared::SparseWorld world({.max_authoritative_columns = 1U});
    shared::AuthoritativeColumnChanges changes;
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    world.readAuthoritativeColumnChanges(0U, changes);
    ASSERT_TRUE(changes.full_resync);
    uint64_t cursor = changes.cursor;
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({4, 5, 0}, identity)));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{4, 5, 0}}));
    cursor = changes.cursor;
    ASSERT_TRUE(world.publishAuthoritativeSurface(makeCanonicalSurface({2, 5, 0}, identity)));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{2, 5, 0}, {4, 5, 0}}));
    cursor = changes.cursor;
    ASSERT_TRUE(world.removeAuthoritativeSurface({2, 5, 0}));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{2, 5, 0}}));
    std::array<shared::EditedChunkRevision, 1U> requirements{{{{2, 5, 24}, 2U}}};
    cursor = changes.cursor;
    uint64_t const revision = world.authoritativeSurfaceRevision();
    ASSERT_TRUE(world.setEditedChunkRequirements({2, 5, 0}, requirements));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), revision);
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{2, 5, 0}}));
    cursor = changes.cursor;
    ASSERT_TRUE(world.publishSurfaceMetadata(makeCanonicalSurface({2, 5, 0}, identity)));
    ASSERT_EQ(world.surfaceMetadataTileCount(), 1U);
    ASSERT_TRUE(world.setEditedChunkRequirements({2, 5, 0}, requirements));
    EXPECT_EQ(world.surfaceMetadataTileCount(), 0U);
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.cursor, cursor);
    ASSERT_TRUE(world.setEditedChunkRequirements({2, 5, 0}, {}));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{2, 5, 0}}));
}

TEST(SparseWorldTest, ResidentContentSnapshotCopiesSortedIdentitiesAndDetectsBorrowedMutation)
{
    shared::SparseWorld world;
    ASSERT_NE(world.ensureChunk({3, 2, 1}), nullptr);
    auto* const borrowed = world.ensureChunk({1, 2, 2});
    ASSERT_NE(borrowed, nullptr);
    std::vector<shared::ResidentChunkContent> contents;
    world.residentChunkContents(contents);
    ASSERT_EQ(contents.size(), 2U);
    EXPECT_EQ(contents[0].coordinate, (shared::ChunkCoordinate{1, 2, 2}));
    EXPECT_EQ(contents[1].coordinate, (shared::ChunkCoordinate{3, 2, 1}));
    auto const original = contents[0].content;
    auto const old_block = borrowed->blockAt({0U, 0U, 1U});
    ASSERT_TRUE(borrowed->setBlock({0U, 0U, 1U}, old_block == shared::Block::Air
        ? shared::Block::Stone : shared::Block::Air));
    EXPECT_EQ(contents[0].content, original);
    world.residentChunkContents(contents);
    EXPECT_NE(contents[0].content, original);
    EXPECT_EQ(contents[0].content, borrowed->contentIdentity());
    ASSERT_TRUE(world.evict({1, 2, 2}));
    world.residentChunkContents(contents);
    ASSERT_EQ(contents.size(), 1U);
    EXPECT_EQ(contents[0].coordinate, (shared::ChunkCoordinate{3, 2, 1}));
}

TEST(SparseWorldTest, ColumnJournalTracksManagedEditsSnapshotsDeltasAndDensePublication)
{
    static constexpr shared::ChunkCoordinate TILE{4, 5, 0};
    static constexpr shared::ChunkCoordinate CHUNK{4, 5, 24};
    static constexpr shared::WorldCoordinate BLOCK{64, 80, 384};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(TILE, identity);
    shared::SparseWorld world;
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    shared::AuthoritativeColumnChanges changes;
    world.readAuthoritativeColumnChanges(0U, changes);
    uint64_t cursor = changes.cursor;
    auto effective = makeCanonicalChunk(surface, CHUNK);
    ASSERT_TRUE(effective.setBlock({0U, 0U, 0U}, shared::Block::Snow));
    ASSERT_TRUE(world.publishMaterializedChunk(effective, 1U, 42U));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{TILE}));
    cursor = changes.cursor;
    ASSERT_TRUE(world.setBlock(BLOCK, shared::Block::Air));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{TILE}));
    cursor = changes.cursor;
    shared::SparseEditSnapshot snapshot{
        .coordinate = CHUNK, .revision = 2U,
        .edits = {{{0U, 0U, 0U}, shared::Block::Snow, 2U}},
    };
    ASSERT_TRUE(world.installEditSnapshot(snapshot, effective));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{TILE}));
    cursor = changes.cursor;
    ASSERT_TRUE(world.applyEditDelta(BLOCK, shared::Block::Air, 2U, 3U, 3U));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{TILE}));
}

TEST(SparseWorldTest, ColumnJournalTracksPreviewAvailabilityWithoutReplacementChurn)
{
    shared::SparseWorld world({.max_resident_chunks = 1U});
    shared::HeightTile preview{.coordinate = {1, 2}};
    shared::AuthoritativeColumnChanges changes;
    ASSERT_TRUE(world.publishPreview({1, 2, 0}, preview, 1U, 42U));
    world.readAuthoritativeColumnChanges(0U, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{1, 2, 0}}));
    uint64_t cursor = changes.cursor;
    ASSERT_TRUE(world.publishPreview({1, 2, 0}, preview, 1U, 42U));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.cursor, cursor);
    ASSERT_NE(world.ensureChunk({3, 4, 1}), nullptr);
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{1, 2, 0}, {3, 4, 0}}));
    ASSERT_TRUE(world.evict({3, 4, 1}));
    ASSERT_TRUE(world.publishPreview({1, 2, 0}, preview, 1U, 42U));
    world.readAuthoritativeColumnChanges(changes.cursor, changes);
    cursor = changes.cursor;
    ASSERT_TRUE(world.evict({1, 2, 0}));
    world.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{1, 2, 0}}));
}

TEST(SparseWorldTest, JournalStorageKeepsWorldBelowDefaultWindowsStackBudget)
{
    EXPECT_LT(sizeof(shared::SparseWorld), 16'384U);
}

TEST(SparseWorldTest, MovedWorldKeepsJournalHistoryAndIndependentInstances)
{
    shared::SparseWorld original{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    shared::EditedChunkRevision const initial{{0, 0, 0}, 1U};
    ASSERT_TRUE(original.setEditedChunkRequirements({0, 0, 0}, std::span{&initial, 1U}));
    shared::AuthoritativeColumnChanges changes;
    original.readAuthoritativeColumnChanges(0U, changes);
    uint64_t const cursor = changes.cursor;
    shared::SparseWorld moved = std::move(original);
    shared::SparseWorld assigned{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    assigned = std::move(moved);
    shared::EditedChunkRevision const first{{1, 0, 0}, 1U};
    ASSERT_TRUE(assigned.setEditedChunkRequirements({1, 0, 0}, std::span{&first, 1U}));
    assigned.readAuthoritativeColumnChanges(cursor, changes);
    EXPECT_FALSE(changes.full_resync);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{1, 0, 0}}));
    shared::SparseWorld independent{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    shared::EditedChunkRevision const second{{2, 0, 0}, 1U};
    ASSERT_TRUE(independent.setEditedChunkRequirements({2, 0, 0}, std::span{&second, 1U}));
    assigned.readAuthoritativeColumnChanges(0U, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{0, 0, 0}, {1, 0, 0}}));
    independent.readAuthoritativeColumnChanges(0U, changes);
    EXPECT_EQ(changes.coordinates, (std::vector<shared::ChunkCoordinate>{{2, 0, 0}}));
}
