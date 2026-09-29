#include <shared/world/ChunkMeshLod.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::array<shared::FaceDirection, 6> DIRECTIONS{
    shared::FaceDirection::NegativeX,
    shared::FaceDirection::PositiveX,
    shared::FaceDirection::NegativeY,
    shared::FaceDirection::PositiveY,
    shared::FaceDirection::NegativeZ,
    shared::FaceDirection::PositiveZ,
};

struct FaceCell final {
    shared::Block material = shared::Block::Air;
    bool used = false;
};

using FaceGrid = std::array<FaceCell, 6U * shared::Chunk::SIDE_LENGTH
    * shared::Chunk::SIDE_LENGTH * shared::Chunk::SIDE_LENGTH>;

[[nodiscard]]
constexpr uint32_t detailIndex(shared::MeshDetail const detail) noexcept
{
    return static_cast<uint32_t>(detail);
}

[[nodiscard]]
constexpr uint32_t faceCellIndex(
    shared::FaceDirection const direction,
    uint8_t const plane,
    uint8_t const v,
    uint8_t const u
) noexcept {
    uint32_t const side = shared::Chunk::SIDE_LENGTH;
    return (((static_cast<uint32_t>(direction) * side + plane) * side + v) * side) + u;
}

[[nodiscard]]
bool faceRowMatches(
    FaceGrid const& grid,
    shared::FaceDirection const direction,
    uint8_t const plane,
    uint8_t const v,
    uint8_t const u,
    uint8_t const width,
    shared::Block const material
) noexcept
{
    for (uint8_t offset = 0U; offset < width; ++offset) {
        FaceCell const& next = grid[faceCellIndex(direction, plane, v, u + offset)];
        if (next.material != material || next.used) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
constexpr std::array<uint8_t, 3> faceGridCoordinates(shared::MeshFace const& face) noexcept
{
    switch (face.direction) {
    case shared::FaceDirection::NegativeX:
    case shared::FaceDirection::PositiveX:
        return { face.local_origin.x, face.local_origin.z, face.local_origin.y };
    case shared::FaceDirection::NegativeY:
    case shared::FaceDirection::PositiveY:
        return { face.local_origin.y, face.local_origin.z, face.local_origin.x };
    case shared::FaceDirection::NegativeZ:
    case shared::FaceDirection::PositiveZ:
        return { face.local_origin.z, face.local_origin.y, face.local_origin.x };
    }
    return {};
}

[[nodiscard]]
constexpr shared::BlockCoordinate faceOrigin(
    shared::FaceDirection const direction,
    uint8_t const plane,
    uint8_t const v,
    uint8_t const u
) noexcept {
    switch (direction) {
    case shared::FaceDirection::NegativeX:
    case shared::FaceDirection::PositiveX:
        return { .x = plane, .y = u, .z = v };
    case shared::FaceDirection::NegativeY:
    case shared::FaceDirection::PositiveY:
        return { .x = u, .y = plane, .z = v };
    case shared::FaceDirection::NegativeZ:
    case shared::FaceDirection::PositiveZ:
        return { .x = u, .y = v, .z = plane };
    }
    return {};
}

[[nodiscard]]
shared::ChunkMeshBounds meshBounds() noexcept
{
    return {
        .minimum = { 0U, 0U, 0U },
        .maximum = {
            shared::Chunk::SIDE_LENGTH,
            shared::Chunk::SIDE_LENGTH,
            shared::Chunk::SIDE_LENGTH,
        },
    };
}

[[nodiscard]]
shared::MeshDetail requestedDetail(
    shared::MeshLodSelectionInput const input,
    std::optional<shared::MeshDetail> const current
)
{
    if (input.coarse_distance_squared < input.fine_distance_squared) {
        throw std::invalid_argument{ "coarse LOD distance must not be closer than fine LOD distance" };
    }
    if (current == shared::MeshDetail::Fine) {
        return input.distance_squared >= input.coarse_distance_squared
            ? shared::MeshDetail::Coarse
            : shared::MeshDetail::Fine;
    }
    if (current == shared::MeshDetail::Coarse) {
        return input.distance_squared <= input.fine_distance_squared
            ? shared::MeshDetail::Fine
            : shared::MeshDetail::Coarse;
    }
    return input.distance_squared >= input.coarse_distance_squared
        ? shared::MeshDetail::Coarse
        : shared::MeshDetail::Fine;
}

[[nodiscard]]
std::optional<shared::MeshDetail> availableFallback(
    shared::MeshDetail const requested,
    shared::ChunkMeshLodSet const& available
) noexcept {
    if (available.variant(requested)) {
        return requested;
    }
    shared::MeshDetail const alternate = requested == shared::MeshDetail::Fine
        ? shared::MeshDetail::Coarse
        : shared::MeshDetail::Fine;
    if (available.variant(alternate)) {
        return alternate;
    }
    return std::nullopt;
}

} // namespace

namespace shared {

std::optional<std::reference_wrapper<ChunkMeshVariant const>> ChunkMeshLodSet::variant(
    MeshDetail const detail
) const noexcept
{
    uint32_t const index = detailIndex(detail);
    if (index >= variants.size() || !variants[index]) {
        return std::nullopt;
    }
    return std::cref(*variants[index]);
}

void ChunkMeshLodSet::remove(MeshDetail const detail) noexcept
{
    uint32_t const index = detailIndex(detail);
    if (index < variants.size()) {
        variants[index].reset();
    }
}

ChunkMeshLodSet ChunkMeshLodBuilder::build(ChunkMesh const& mesh) const
{
    if (mesh.faces.size() > ChunkMesh::MAXIMUM_CHUNK_FACE_COUNT) {
        throw std::invalid_argument{ "chunk mesh exceeds the maximum face count" };
    }

    ChunkMeshLodSet result;
    FaceGrid grid{};
    ChunkMeshVariant fine{
        .detail = MeshDetail::Fine,
        .content_identity = mesh.content_identity,
        .bounds = meshBounds(),
        .quads = {},
    };
    fine.quads.reserve(mesh.faces.size());
    for (MeshFace const& face : mesh.faces) {
        if (static_cast<uint32_t>(face.direction) >= DIRECTIONS.size()) {
            throw std::invalid_argument{ "chunk mesh contains an invalid face direction" };
        }
        if (!Chunk::isValid(face.local_origin) || face.material == Block::Air) {
            throw std::invalid_argument{ "chunk mesh contains an invalid face" };
        }
        std::array<uint8_t, 3> const coordinates = faceGridCoordinates(face);
        FaceCell& cell = grid[faceCellIndex(face.direction, coordinates[0], coordinates[1], coordinates[2])];
        if (cell.material != Block::Air) {
            throw std::invalid_argument{ "chunk mesh contains duplicate face coordinates" };
        }
        cell.material = face.material;
        fine.quads.push_back({
            .origin = face.local_origin,
            .direction = face.direction,
            .material = face.material,
            .width = 1U,
            .height = 1U,
        });
    }

    ChunkMeshVariant coarse{
        .detail = MeshDetail::Coarse,
        .content_identity = mesh.content_identity,
        .bounds = meshBounds(),
        .quads = {},
    };
    coarse.quads.reserve(mesh.faces.size());
    uint8_t const side = Chunk::SIDE_LENGTH;
    for (FaceDirection const direction : DIRECTIONS) {
        for (uint8_t plane = 0U; plane < side; ++plane) {
            for (uint8_t v = 0U; v < side; ++v) {
                for (uint8_t u = 0U; u < side; ++u) {
                    FaceCell& start = grid[faceCellIndex(direction, plane, v, u)];
                    if (start.material == Block::Air || start.used) {
                        continue;
                    }
                    uint8_t width = 1U;
                    while (u + width < side) {
                        FaceCell const& next = grid[faceCellIndex(direction, plane, v, u + width)];
                        if (next.material != start.material || next.used) {
                            break;
                        }
                        ++width;
                    }
                    uint8_t height = 1U;
                    while (v + height < side) {
                        if (!faceRowMatches(grid, direction, plane, v + height, u, width, start.material)) {
                            break;
                        }
                        ++height;
                    }
                    for (uint8_t v_offset = 0U; v_offset < height; ++v_offset) {
                        for (uint8_t u_offset = 0U; u_offset < width; ++u_offset) {
                            grid[faceCellIndex(direction, plane, v + v_offset, u + u_offset)].used = true;
                        }
                    }
                    coarse.quads.push_back({
                        .origin = faceOrigin(direction, plane, v, u),
                        .direction = direction,
                        .material = start.material,
                        .width = width,
                        .height = height,
                    });
                }
            }
        }
    }

    if (fine.quads.size() > ChunkMesh::MAXIMUM_CHUNK_FACE_COUNT
        || coarse.quads.size() > ChunkMesh::MAXIMUM_CHUNK_FACE_COUNT) {
        throw std::logic_error{ "chunk mesh LOD exceeded its fixed storage bound" };
    }
    result.variants[detailIndex(MeshDetail::Coarse)] = std::move(coarse);
    result.variants[detailIndex(MeshDetail::Fine)] = std::move(fine);
    return result;
}

MeshLodSelection MeshLodSelector::select(
    MeshLodSelectionInput const input,
    ChunkMeshLodSet const& available
)
{
    MeshDetail const requested = requestedDetail(input, m_current);
    std::optional<MeshDetail> const selected = availableFallback(requested, available);
    if (!selected) {
        throw std::invalid_argument{ "chunk mesh LOD set has no available variants" };
    }
    m_current = selected;
    return {
        .requested = requested,
        .selected = *selected,
        .used_fallback = requested != *selected,
    };
}

std::optional<MeshDetail> MeshLodSelector::current() const noexcept
{
    return m_current;
}

} // namespace shared
