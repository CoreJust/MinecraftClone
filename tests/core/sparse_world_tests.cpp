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
    EXPECT_EQ(authoritative_column->dense_section_mask, 0U);
    EXPECT_EQ(authoritative_column->dense_blocks, nullptr);
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
    EXPECT_EQ(column->dense_section_mask, uint64_t{1} << 56U);
    EXPECT_EQ(column->highest_solid_z, 900U);
    EXPECT_EQ(column->materialAt(900), shared::Block::Snow);
    EXPECT_EQ(column->materialAt(899), shared::Block::Air);
    EXPECT_EQ(world.queryBlock({64, 80, 900}).block, shared::Block::Snow);
    EXPECT_EQ(world.queryBlock({65, 80, 900}).block, shared::Block::Air);
    EXPECT_GT(world.authoritativeSurfaceRevision(), before);
    EXPECT_FALSE(world.evict({4, 5, 56}));
    world.setWorldIdentity(2U, 42U);
    EXPECT_FALSE(world.authoritativeColumn(64, 80).has_value());
    EXPECT_EQ(world.queryBlock({64, 80, 900}).state, shared::GenerationState::Unknown);
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
    EXPECT_FALSE(world.removeAuthoritativeSurface({1, 0, 0}));
    EXPECT_FALSE(world.evict({1, 0, 56}));
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

TEST(SparseWorldTest, AuthorityContentIdentityDetectsDirectDenseEditsAndUnknownBaselines)
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
    EXPECT_EQ(lower_edit_column->dense_section_mask, uint64_t{1} << 24U);

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
    EXPECT_NE(unknown_resident.content_hash, initial.content_hash);
    auto const unknown_column = unknown.authoritativeColumn(64, 80);
    ASSERT_TRUE(unknown_column.has_value());
    if (!unknown_column) {
        return;
    }
    EXPECT_EQ(unknown_column->dense_section_mask, uint64_t{1} << 24U);
    ASSERT_TRUE(unknown.evict({4, 5, 24}));
    auto const unknown_evicted_identity = unknown.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(unknown_evicted_identity.has_value());
    if (!unknown_evicted_identity) {
        return;
    }
    EXPECT_NE(unknown_evicted_identity->content_hash, unknown_resident.content_hash);
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

TEST(SparseWorldTest, ExactAuthorityIdentityMatchDetectsUnknownDenseBaselineEviction)
{
    static constexpr shared::ChunkCoordinate COORDINATE{4, 5, 0};
    static constexpr shared::ChunkCoordinate DENSE_COORDINATE{4, 5, 24};
    auto const identity = testsupport::makeTerrainIdentity(42U, 1U);
    auto const surface = makeCanonicalSurface(COORDINATE, identity);
    shared::SparseWorld world({.seed = 42U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishMaterializedChunk(makeCanonicalChunk(surface, DENSE_COORDINATE), 1U, 42U));
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
