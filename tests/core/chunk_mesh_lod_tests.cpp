#include <shared/world/ChunkMeshLod.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <stdexcept>

namespace {

[[nodiscard]]
uint32_t coveredArea(shared::ChunkMeshVariant const& variant)
{
    return std::accumulate(
        variant.quads.begin(),
        variant.quads.end(),
        0U,
        [](uint32_t total, shared::MeshQuad const& quad) {
            return total + static_cast<uint32_t>(quad.width) * quad.height;
        }
    );
}

} // namespace

TEST(ChunkMeshLodTest, BuildsDeterministicVariantsWithStableBoundsAndRevision)
{
    shared::Chunk chunk = shared::Chunk::makeStoneFixture({ .x = -2, .y = 3, .z = 1 });
    shared::ChunkMesher mesher;
    shared::ChunkMesh const& mesh = mesher.update(chunk);
    shared::ChunkContentIdentity const initial_identity = mesh.content_identity;
    shared::ChunkMeshLodBuilder builder;

    shared::ChunkMeshLodSet const first = builder.build(mesh);
    shared::ChunkMeshLodSet const second = builder.build(mesh);

    ASSERT_TRUE(first.variant(shared::MeshDetail::Fine).has_value());
    ASSERT_TRUE(first.variant(shared::MeshDetail::Coarse).has_value());
    EXPECT_EQ(first.variant(shared::MeshDetail::Fine)->get(), second.variant(shared::MeshDetail::Fine)->get());
    EXPECT_EQ(first.variant(shared::MeshDetail::Coarse)->get(), second.variant(shared::MeshDetail::Coarse)->get());
    shared::ChunkMeshVariant const& fine = first.variant(shared::MeshDetail::Fine)->get();
    shared::ChunkMeshVariant const& repeated_fine = second.variant(shared::MeshDetail::Fine)->get();
    EXPECT_EQ(fine.content_identity, mesh.content_identity);
    EXPECT_EQ(fine.bounds, repeated_fine.bounds);
    EXPECT_EQ(fine.bounds.minimum, (std::array<uint8_t, 3>{ 0, 0, 0 }));
    EXPECT_EQ(fine.bounds.maximum, (std::array<uint8_t, 3>{
        shared::Chunk::SIDE_LENGTH,
        shared::Chunk::SIDE_LENGTH,
        shared::Chunk::SIDE_LENGTH,
    }));
    EXPECT_LE(
        first.variant(shared::MeshDetail::Fine)->get().quads.size()
            + first.variant(shared::MeshDetail::Coarse)->get().quads.size(),
        shared::ChunkMeshLodSet::MAXIMUM_TOTAL_QUAD_COUNT
    );

    ASSERT_TRUE(chunk.setBlock({ .x = 0U, .y = 0U, .z = 0U }, shared::Block::Air));
    shared::ChunkMeshLodSet const revised = builder.build(mesher.update(chunk));
    EXPECT_NE(revised.variant(shared::MeshDetail::Fine)->get().content_identity, initial_identity);
}

TEST(ChunkMeshLodTest, CoarseVariantPreservesSurfaceCoverageAndHiddenFaces)
{
    static constexpr shared::BlockCoordinate LEFT{ .x = 7, .y = 7, .z = 7 };
    static constexpr shared::BlockCoordinate RIGHT{ .x = 8, .y = 7, .z = 7 };
    shared::Chunk chunk;
    ASSERT_TRUE(chunk.setBlock(LEFT, shared::Block::Stone));
    ASSERT_TRUE(chunk.setBlock(RIGHT, shared::Block::Stone));
    shared::ChunkMesher mesher;
    shared::ChunkMeshLodBuilder builder;

    shared::ChunkMeshLodSet const set = builder.build(mesher.update(chunk));
    shared::ChunkMeshVariant const& fine = set.variant(shared::MeshDetail::Fine)->get();
    shared::ChunkMeshVariant const& coarse = set.variant(shared::MeshDetail::Coarse)->get();

    EXPECT_EQ(coveredArea(fine), 10U);
    EXPECT_EQ(coveredArea(coarse), coveredArea(fine));
    EXPECT_LT(coarse.quads.size(), fine.quads.size());
    EXPECT_TRUE(std::ranges::none_of(coarse.quads, [](shared::MeshQuad const& quad) {
        return (quad.origin == LEFT && quad.direction == shared::FaceDirection::PositiveX)
            || (quad.origin == RIGHT && quad.direction == shared::FaceDirection::NegativeX);
    }));
}

TEST(ChunkMeshLodTest, CoarseVariantRetainsFaceMaterialsAndDirections)
{
    static constexpr shared::BlockCoordinate LOWER{ .x = 7, .y = 7, .z = 7 };
    static constexpr shared::BlockCoordinate UPPER{ .x = 7, .y = 7, .z = 8 };
    shared::Chunk chunk;
    ASSERT_TRUE(chunk.setBlock(LOWER, shared::Block::Stone));
    ASSERT_TRUE(chunk.setBlock(UPPER, shared::Block::Stone));
    shared::ChunkMesher mesher;
    shared::ChunkMeshLodBuilder builder;

    shared::ChunkMeshLodSet const set = builder.build(mesher.update(chunk));
    shared::ChunkMeshVariant const& coarse = set.variant(shared::MeshDetail::Coarse)->get();

    EXPECT_EQ(coveredArea(coarse), 10U);
    EXPECT_TRUE(std::ranges::all_of(coarse.quads, [](shared::MeshQuad const& quad) {
        return quad.material == shared::Block::Stone;
    }));
    EXPECT_TRUE(std::ranges::any_of(coarse.quads, [](shared::MeshQuad const& quad) {
        return quad.origin == LOWER && quad.direction == shared::FaceDirection::NegativeZ;
    }));
    EXPECT_TRUE(std::ranges::any_of(coarse.quads, [](shared::MeshQuad const& quad) {
        return quad.origin == UPPER && quad.direction == shared::FaceDirection::PositiveZ;
    }));
}

TEST(ChunkMeshLodTest, CoarseVariantPreservesNeighborChunkSeams)
{
    static constexpr shared::BlockCoordinate EDGE_BLOCK{
        .x = static_cast<uint8_t>(shared::Chunk::SIDE_LENGTH - 1U),
        .y = 7U,
        .z = 7U,
    };
    shared::Chunk chunk;
    ASSERT_TRUE(chunk.setBlock(EDGE_BLOCK, shared::Block::Stone));
    shared::ChunkNeighborFaces neighbors;
    std::optional<std::array<shared::Block, shared::Chunk::FACE_BLOCK_COUNT>>& positive_x =
        neighbors.faces[static_cast<uint32_t>(shared::FaceDirection::PositiveX)];
    positive_x.emplace();
    positive_x->fill(shared::Block::Stone);
    shared::ChunkMeshLodBuilder builder;
    shared::ChunkMesh const mesh = shared::ChunkMesher{}.update(chunk, neighbors);
    shared::ChunkMeshLodSet const lod_set = builder.build(mesh);
    shared::ChunkMeshVariant const& coarse = lod_set.variant(shared::MeshDetail::Coarse)->get();

    EXPECT_EQ(coveredArea(coarse), 5U);
    EXPECT_TRUE(std::ranges::none_of(coarse.quads, [](shared::MeshQuad const& quad) {
        return quad.origin.x == shared::Chunk::SIDE_LENGTH - 1U
            && quad.direction == shared::FaceDirection::PositiveX;
    }));
}

TEST(ChunkMeshLodTest, SelectorUsesHysteresisAndFallsBackToAvailableDetail)
{
    shared::Chunk chunk = shared::Chunk::makeStoneFixture();
    shared::ChunkMesher mesher;
    shared::ChunkMeshLodSet set = shared::ChunkMeshLodBuilder{}.build(mesher.update(chunk));
    shared::MeshLodSelector selector;
    shared::MeshLodSelectionInput const far{ .distance_squared = 100U, .coarse_distance_squared = 80U,
        .fine_distance_squared = 40U };
    shared::MeshLodSelectionInput const hysteresis_band{ .distance_squared = 60U,
        .coarse_distance_squared = 80U, .fine_distance_squared = 40U };
    shared::MeshLodSelectionInput const near{ .distance_squared = 20U, .coarse_distance_squared = 80U,
        .fine_distance_squared = 40U };

    EXPECT_EQ(selector.select(far, set).selected, shared::MeshDetail::Coarse);
    EXPECT_EQ(selector.select(hysteresis_band, set).selected, shared::MeshDetail::Coarse);
    EXPECT_EQ(selector.select(near, set).selected, shared::MeshDetail::Fine);
    set.remove(shared::MeshDetail::Coarse);
    shared::MeshLodSelection const fallback = selector.select(far, set);
    EXPECT_EQ(fallback.requested, shared::MeshDetail::Coarse);
    EXPECT_EQ(fallback.selected, shared::MeshDetail::Fine);
    EXPECT_TRUE(fallback.used_fallback);
}

TEST(ChunkMeshLodTest, SelectingAcrossFramesDoesNotRemeshTheChunk)
{
    shared::Chunk chunk = shared::Chunk::makeStoneFixture();
    shared::ChunkMesher mesher;
    shared::ChunkMesh const& mesh = mesher.update(chunk);
    shared::ChunkMeshLodSet const set = shared::ChunkMeshLodBuilder{}.build(mesh);
    shared::MeshLodSelector selector;

    for (uint64_t distance_squared = 0; distance_squared < 200U; ++distance_squared) {
        static_cast<void>(selector.select({
            .distance_squared = distance_squared,
            .coarse_distance_squared = 100U,
            .fine_distance_squared = 64U,
        }, set));
    }

    EXPECT_EQ(mesher.buildCount(), 1U);
    EXPECT_EQ(&mesher.update(chunk), &mesh);
    EXPECT_EQ(mesher.buildCount(), 1U);
}

TEST(ChunkMeshLodTest, RejectsUnknownFaceDirections)
{
    static constexpr shared::BlockCoordinate BLOCK{ .x = 7U, .y = 7U, .z = 7U };
    shared::Chunk chunk;
    ASSERT_TRUE(chunk.setBlock(BLOCK, shared::Block::Stone));
    shared::ChunkMesh mesh = shared::ChunkMesher{}.update(chunk);
    ASSERT_FALSE(mesh.faces.empty());
    mesh.faces.front().direction = static_cast<shared::FaceDirection>(0xffU);

    EXPECT_THROW(static_cast<void>(shared::ChunkMeshLodBuilder{}.build(mesh)), std::invalid_argument);
}
