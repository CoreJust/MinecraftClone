#include <shared/world/ChunkMesher.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace {

[[nodiscard]]
shared::ChunkNeighborFaces noNeighbors()
{
    return {};
}

void fill(shared::Chunk& chunk, shared::Block const block)
{
    for (uint8_t z = 0; z < shared::Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0; x < shared::Chunk::SIDE_LENGTH; ++x) {
                ASSERT_TRUE(chunk.setBlock({ .x = x, .y = y, .z = z }, block));
            }
        }
    }
}

[[nodiscard]]
bool hasFace(
    shared::ChunkMesh const& mesh,
    shared::BlockCoordinate const local_origin,
    shared::FaceDirection const direction
)
{
    return std::ranges::any_of(mesh.faces, [local_origin, direction](shared::MeshFace const& face) {
        return face.local_origin == local_origin && face.direction == direction;
    });
}

[[nodiscard]]
std::array<int8_t, 3> cross(
    std::array<int8_t, 3> const lhs,
    std::array<int8_t, 3> const rhs
)
{
    return {
        static_cast<int8_t>(lhs[1] * rhs[2] - lhs[2] * rhs[1]),
        static_cast<int8_t>(lhs[2] * rhs[0] - lhs[0] * rhs[2]),
        static_cast<int8_t>(lhs[0] * rhs[1] - lhs[1] * rhs[0]),
    };
}

[[nodiscard]]
std::array<int8_t, 3> difference(
    shared::FaceVertexOffset const lhs,
    shared::FaceVertexOffset const rhs
)
{
    return {
        static_cast<int8_t>(lhs.x - rhs.x),
        static_cast<int8_t>(lhs.y - rhs.y),
        static_cast<int8_t>(lhs.z - rhs.z),
    };
}

} // namespace

TEST(ChunkMesherTest, EmptyChunkBuildsNoFacesAndReusesTheMesh)
{
    shared::Chunk chunk;
    shared::ChunkMesher mesher;

    shared::ChunkMesh const& mesh = mesher.update(chunk);
    EXPECT_TRUE(mesh.faces.empty());
    EXPECT_EQ(mesher.buildCount(), 1U);

    shared::ChunkMesh const& reused_mesh = mesher.update(chunk);
    EXPECT_EQ(&reused_mesh, &mesh);
    EXPECT_EQ(mesher.buildCount(), 1U);
}

TEST(ChunkMesherTest, SingleBlockEmitsSixOutwardCounterClockwiseFaces)
{
    static constexpr shared::BlockCoordinate BLOCK{ .x = 4, .y = 5, .z = 6 };
    static constexpr std::array<shared::FaceDirection, 6> DIRECTIONS{
        shared::FaceDirection::NegativeX,
        shared::FaceDirection::PositiveX,
        shared::FaceDirection::NegativeY,
        shared::FaceDirection::PositiveY,
        shared::FaceDirection::NegativeZ,
        shared::FaceDirection::PositiveZ,
    };
    shared::Chunk chunk;
    shared::ChunkMesher mesher;
    ASSERT_TRUE(chunk.setBlock(BLOCK, shared::Block::Stone));

    shared::ChunkMesh const& mesh = mesher.update(chunk);
    ASSERT_EQ(mesh.faces.size(), 6U);
    for (shared::FaceDirection const direction : DIRECTIONS) {
        EXPECT_TRUE(hasFace(mesh, BLOCK, direction));
        std::array<shared::FaceVertexOffset, 4> const offsets = shared::faceVertexOffsets(direction);
        std::array<int8_t, 3> const first_edge = difference(offsets[1], offsets[0]);
        std::array<int8_t, 3> const second_edge = difference(offsets[2], offsets[0]);
        EXPECT_EQ(cross(first_edge, second_edge), shared::faceNormal(direction));
    }
}

TEST(ChunkMesherTest, AdjacentBlocksHideTheirSharedFaces)
{
    static constexpr shared::BlockCoordinate LEFT{ .x = 7, .y = 7, .z = 7 };
    static constexpr shared::BlockCoordinate RIGHT{ .x = 8, .y = 7, .z = 7 };
    shared::Chunk chunk;
    shared::ChunkMesher mesher;
    ASSERT_TRUE(chunk.setBlock(LEFT, shared::Block::Stone));
    ASSERT_TRUE(chunk.setBlock(RIGHT, shared::Block::Stone));

    shared::ChunkMesh const& mesh = mesher.update(chunk);
    EXPECT_EQ(mesh.faces.size(), 10U);
    EXPECT_FALSE(hasFace(mesh, LEFT, shared::FaceDirection::PositiveX));
    EXPECT_FALSE(hasFace(mesh, RIGHT, shared::FaceDirection::NegativeX));
}

TEST(ChunkMesherTest, SolidChunkHidesFacesAgainstSolidNeighborBoundary)
{
    static constexpr uint32_t OUTER_FACE_COUNT = 6U * shared::Chunk::FACE_BLOCK_COUNT;
    shared::Chunk chunk;
    shared::ChunkMesher mesher;
    fill(chunk, shared::Block::Stone);

    shared::ChunkMesh const& without_neighbor = mesher.update(chunk);
    ASSERT_EQ(without_neighbor.faces.size(), OUTER_FACE_COUNT);
    EXPECT_LE(without_neighbor.faces.size(), shared::ChunkMesh::MAXIMUM_FACE_COUNT);

    shared::ChunkNeighborFaces neighbors = noNeighbors();
    std::optional<std::array<shared::Block, shared::Chunk::FACE_BLOCK_COUNT>>& positive_x =
        neighbors.faces[static_cast<uint32_t>(shared::FaceDirection::PositiveX)];
    positive_x.emplace();
    positive_x->fill(shared::Block::Stone);

    shared::ChunkMesh const& with_neighbor = mesher.update(chunk, neighbors);
    EXPECT_EQ(with_neighbor.faces.size(), OUTER_FACE_COUNT - shared::Chunk::FACE_BLOCK_COUNT);
    EXPECT_EQ(mesher.buildCount(), 2U);
    EXPECT_TRUE(std::ranges::none_of(with_neighbor.faces, [](shared::MeshFace const& face) {
        return face.local_origin.x == shared::Chunk::SIDE_LENGTH - 1
            && face.direction == shared::FaceDirection::PositiveX;
    }));
}

TEST(ChunkMesherTest, InputChangesInvalidateWhileRepeatedInputsDoNot)
{
    static constexpr shared::BlockCoordinate BLOCK{ .x = 1, .y = 2, .z = 3 };
    shared::Chunk chunk;
    shared::ChunkMesher mesher;

    static_cast<void>(mesher.update(chunk));
    EXPECT_EQ(mesher.buildCount(), 1U);
    static_cast<void>(mesher.update(chunk));
    EXPECT_EQ(mesher.buildCount(), 1U);

    ASSERT_TRUE(chunk.setBlock(BLOCK, shared::Block::Stone));
    shared::ChunkMesh const& changed_mesh = mesher.update(chunk);
    EXPECT_EQ(changed_mesh.content_identity, chunk.contentIdentity());
    EXPECT_EQ(mesher.buildCount(), 2U);

    static_cast<void>(mesher.update(chunk));
    EXPECT_EQ(mesher.buildCount(), 2U);
}
