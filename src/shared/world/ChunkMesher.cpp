#include <shared/world/ChunkMesher.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace {

constexpr std::array<shared::FaceDirection, 6> DIRECTIONS{
    shared::FaceDirection::NegativeX,
    shared::FaceDirection::PositiveX,
    shared::FaceDirection::NegativeY,
    shared::FaceDirection::PositiveY,
    shared::FaceDirection::NegativeZ,
    shared::FaceDirection::PositiveZ,
};

[[nodiscard]]
constexpr uint32_t directionIndex(shared::FaceDirection const direction) noexcept
{
    return static_cast<uint32_t>(direction);
}

[[nodiscard]]
constexpr uint32_t blockIndex(shared::BlockCoordinate const coordinate) noexcept
{
    return static_cast<uint32_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH
        + static_cast<uint32_t>(coordinate.y) * shared::Chunk::SIDE_LENGTH
        + coordinate.x;
}

[[nodiscard]]
constexpr std::optional<shared::BlockCoordinate> adjacentCoordinate(
    shared::BlockCoordinate const coordinate,
    shared::FaceDirection const direction
) noexcept {
    switch (direction) {
    case shared::FaceDirection::NegativeX:
        if (coordinate.x == 0) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = static_cast<uint8_t>(coordinate.x - 1),
            .y = coordinate.y,
            .z = coordinate.z,
        };
    case shared::FaceDirection::PositiveX:
        if (coordinate.x + 1 == shared::Chunk::SIDE_LENGTH) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = static_cast<uint8_t>(coordinate.x + 1),
            .y = coordinate.y,
            .z = coordinate.z,
        };
    case shared::FaceDirection::NegativeY:
        if (coordinate.y == 0) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = coordinate.x,
            .y = static_cast<uint8_t>(coordinate.y - 1),
            .z = coordinate.z,
        };
    case shared::FaceDirection::PositiveY:
        if (coordinate.y + 1 == shared::Chunk::SIDE_LENGTH) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = coordinate.x,
            .y = static_cast<uint8_t>(coordinate.y + 1),
            .z = coordinate.z,
        };
    case shared::FaceDirection::NegativeZ:
        if (coordinate.z == 0) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = coordinate.x,
            .y = coordinate.y,
            .z = static_cast<uint8_t>(coordinate.z - 1),
        };
    case shared::FaceDirection::PositiveZ:
        if (coordinate.z + 1 == shared::Chunk::SIDE_LENGTH) {
            return std::nullopt;
        }
        return shared::BlockCoordinate{
            .x = coordinate.x,
            .y = coordinate.y,
            .z = static_cast<uint8_t>(coordinate.z + 1),
        };
    }
    return std::nullopt;
}

[[nodiscard]]
constexpr uint32_t boundaryBlockIndex(
    shared::BlockCoordinate const coordinate,
    shared::FaceDirection const direction
) noexcept {
    switch (direction) {
    case shared::FaceDirection::NegativeX:
    case shared::FaceDirection::PositiveX:
        return static_cast<uint32_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH + coordinate.y;
    case shared::FaceDirection::NegativeY:
    case shared::FaceDirection::PositiveY:
        return static_cast<uint32_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH + coordinate.x;
    case shared::FaceDirection::NegativeZ:
    case shared::FaceDirection::PositiveZ:
        return static_cast<uint32_t>(coordinate.y) * shared::Chunk::SIDE_LENGTH + coordinate.x;
    }
    return 0;
}

[[nodiscard]]
bool isExposed(
    std::array<shared::Block, shared::Chunk::BLOCK_COUNT> const& blocks,
    shared::ChunkNeighborFaces const& neighbors,
    shared::BlockCoordinate const coordinate,
    shared::FaceDirection const direction
) noexcept {
    std::optional<shared::BlockCoordinate> const adjacent = adjacentCoordinate(coordinate, direction);
    if (adjacent) {
        return blocks[blockIndex(*adjacent)] == shared::Block::Air;
    }

    std::optional<std::array<shared::Block, shared::Chunk::FACE_BLOCK_COUNT>> const& neighbor =
        neighbors.faces[directionIndex(direction)];
    return !neighbor || (*neighbor)[boundaryBlockIndex(coordinate, direction)] == shared::Block::Air;
}

} // namespace

namespace shared {

ChunkMesh const& ChunkMesher::update(Chunk const& chunk, ChunkNeighborFaces const& neighbors)
{
    InputSnapshot const input = snapshot(chunk, neighbors);
    if (m_input && *m_input == input) {
        return m_mesh;
    }

    m_mesh = build(input);
    m_input = input;
    ++m_build_count;
    return m_mesh;
}

uint64_t ChunkMesher::buildCount() const noexcept
{
    return m_build_count;
}

ChunkMesher::InputSnapshot ChunkMesher::snapshot(
    Chunk const& chunk,
    ChunkNeighborFaces const& neighbors
)
{
    InputSnapshot input{
        .coordinate = chunk.coordinate(),
        .content_identity = chunk.contentIdentity(),
        .blocks = {},
        .neighbors = neighbors,
    };
    for (uint8_t z = 0; z < Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0; y < Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0; x < Chunk::SIDE_LENGTH; ++x) {
                BlockCoordinate const coordinate{ .x = x, .y = y, .z = z };
                input.blocks[blockIndex(coordinate)] = chunk.blockAt(coordinate).value();
            }
        }
    }
    return input;
}

ChunkMesh ChunkMesher::build(InputSnapshot const& input)
{
    ChunkMesh mesh{
        .coordinate = input.coordinate,
        .content_identity = input.content_identity,
        .faces = {},
    };
    mesh.faces.reserve(ChunkMesh::MAXIMUM_FACE_COUNT);

    for (uint8_t z = 0; z < Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0; y < Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0; x < Chunk::SIDE_LENGTH; ++x) {
                BlockCoordinate const coordinate{ .x = x, .y = y, .z = z };
                Block const material = input.blocks[blockIndex(coordinate)];
                if (material == Block::Air) {
                    continue;
                }
                for (FaceDirection const direction : DIRECTIONS) {
                    if (isExposed(input.blocks, input.neighbors, coordinate, direction)) {
                        mesh.faces.emplace_back(MeshFace{
                            .local_origin = coordinate,
                            .direction = direction,
                            .material = material,
                        });
                    }
                }
            }
        }
    }
    return mesh;
}

} // namespace shared
