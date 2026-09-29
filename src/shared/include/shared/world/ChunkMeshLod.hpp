#pragma once

#include <shared/world/ChunkMesher.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace shared {

enum class MeshDetail : uint8_t {
    Coarse,
    Fine,
};

struct ChunkMeshBounds final {
    std::array<uint8_t, 3> minimum;
    std::array<uint8_t, 3> maximum;

    constexpr bool operator==(ChunkMeshBounds const&) const noexcept = default;
};

struct MeshQuad final {
    BlockCoordinate origin;
    FaceDirection direction;
    Block material;
    uint8_t width;
    uint8_t height;

    constexpr bool operator==(MeshQuad const&) const noexcept = default;
};

struct ChunkMeshVariant final {
    MeshDetail detail;
    ChunkContentIdentity content_identity;
    ChunkMeshBounds bounds;
    std::vector<MeshQuad> quads;

    bool operator==(ChunkMeshVariant const&) const noexcept = default;
};

struct ChunkMeshLodSet final {
    static constexpr uint32_t MAXIMUM_VARIANT_COUNT = 2U;
    static constexpr uint32_t MAXIMUM_TOTAL_QUAD_COUNT =
        ChunkMesh::MAXIMUM_CHUNK_FACE_COUNT * MAXIMUM_VARIANT_COUNT;

    std::array<std::optional<ChunkMeshVariant>, MAXIMUM_VARIANT_COUNT> variants;

    [[nodiscard]]
    std::optional<std::reference_wrapper<ChunkMeshVariant const>> variant(MeshDetail detail) const noexcept;

    void remove(MeshDetail detail) noexcept;
};

class ChunkMeshLodBuilder final {
public:
    [[nodiscard]]
    ChunkMeshLodSet build(ChunkMesh const& mesh) const;
};

struct MeshLodSelectionInput final {
    uint64_t distance_squared;
    uint64_t coarse_distance_squared;
    uint64_t fine_distance_squared;
};

struct MeshLodSelection final {
    MeshDetail requested;
    MeshDetail selected;
    bool used_fallback;
};

class MeshLodSelector final {
public:
    [[nodiscard]]
    MeshLodSelection select(MeshLodSelectionInput input, ChunkMeshLodSet const& available);

    [[nodiscard]]
    std::optional<MeshDetail> current() const noexcept;

private:
    std::optional<MeshDetail> m_current;
};

} // namespace shared
