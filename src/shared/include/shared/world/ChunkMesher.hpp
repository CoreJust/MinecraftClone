#pragma once

#include <shared/world/Chunk.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace shared {

enum class FaceDirection : uint8_t {
    NegativeX,
    PositiveX,
    NegativeY,
    PositiveY,
    NegativeZ,
    PositiveZ,
};

struct FaceVertexOffset final {
    uint8_t x;
    uint8_t y;
    uint8_t z;

    constexpr bool operator==(FaceVertexOffset const&) const noexcept = default;
};

struct MeshFace final {
    BlockCoordinate local_origin;
    FaceDirection direction;
    Block material;

    constexpr bool operator==(MeshFace const&) const noexcept = default;
};

struct ChunkMesh final {
    static constexpr uint32_t MAXIMUM_FACE_COUNT = Chunk::BLOCK_COUNT * 6;

    ChunkContentIdentity content_identity;
    std::vector<MeshFace> faces;
};

struct ChunkNeighborFaces final {
    std::array<std::optional<std::array<Block, Chunk::FACE_BLOCK_COUNT>>, 6> faces;

    constexpr bool operator==(ChunkNeighborFaces const&) const noexcept = default;
};

[[nodiscard]]
constexpr std::array<int8_t, 3> faceNormal(FaceDirection const direction) noexcept
{
    switch (direction) {
    case FaceDirection::NegativeX:
        return { -1, 0, 0 };
    case FaceDirection::PositiveX:
        return { 1, 0, 0 };
    case FaceDirection::NegativeY:
        return { 0, -1, 0 };
    case FaceDirection::PositiveY:
        return { 0, 1, 0 };
    case FaceDirection::NegativeZ:
        return { 0, 0, -1 };
    case FaceDirection::PositiveZ:
        return { 0, 0, 1 };
    }
    return { 0, 0, 0 };
}

[[nodiscard]]
constexpr std::array<FaceVertexOffset, 4> faceVertexOffsets(
    FaceDirection const direction
) noexcept {
    switch (direction) {
    case FaceDirection::NegativeX:
        return { {
            { .x = 0, .y = 0, .z = 0 },
            { .x = 0, .y = 0, .z = 1 },
            { .x = 0, .y = 1, .z = 1 },
            { .x = 0, .y = 1, .z = 0 },
        } };
    case FaceDirection::PositiveX:
        return { {
            { .x = 1, .y = 0, .z = 0 },
            { .x = 1, .y = 1, .z = 0 },
            { .x = 1, .y = 1, .z = 1 },
            { .x = 1, .y = 0, .z = 1 },
        } };
    case FaceDirection::NegativeY:
        return { {
            { .x = 0, .y = 0, .z = 0 },
            { .x = 1, .y = 0, .z = 0 },
            { .x = 1, .y = 0, .z = 1 },
            { .x = 0, .y = 0, .z = 1 },
        } };
    case FaceDirection::PositiveY:
        return { {
            { .x = 0, .y = 1, .z = 0 },
            { .x = 0, .y = 1, .z = 1 },
            { .x = 1, .y = 1, .z = 1 },
            { .x = 1, .y = 1, .z = 0 },
        } };
    case FaceDirection::NegativeZ:
        return { {
            { .x = 0, .y = 0, .z = 0 },
            { .x = 0, .y = 1, .z = 0 },
            { .x = 1, .y = 1, .z = 0 },
            { .x = 1, .y = 0, .z = 0 },
        } };
    case FaceDirection::PositiveZ:
        return { {
            { .x = 0, .y = 0, .z = 1 },
            { .x = 1, .y = 0, .z = 1 },
            { .x = 1, .y = 1, .z = 1 },
            { .x = 0, .y = 1, .z = 1 },
        } };
    }
    return {};
}

class ChunkMesher final {
public:
    [[nodiscard]]
    ChunkMesh const& update(
        Chunk const& chunk,
        ChunkNeighborFaces const& neighbors = {}
    );

    [[nodiscard]]
    uint64_t buildCount() const noexcept;

private:
    struct InputSnapshot final {
        ChunkContentIdentity content_identity;
        std::array<Block, Chunk::BLOCK_COUNT> blocks;
        ChunkNeighborFaces neighbors;

        constexpr bool operator==(InputSnapshot const&) const noexcept = default;
    };

    [[nodiscard]]
    static InputSnapshot snapshot(Chunk const& chunk, ChunkNeighborFaces const& neighbors);

    [[nodiscard]]
    static ChunkMesh build(InputSnapshot const& input);

private:
    std::optional<InputSnapshot> m_input;
    ChunkMesh m_mesh{};
    uint64_t m_build_count = 0;
};

} // namespace shared
