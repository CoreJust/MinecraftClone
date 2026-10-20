#include <shared/world/WorldCoverage.hpp>

#include <shared/world/SparseWorld.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr int64_t SUBCELLS_PER_BLOCK = shared::SUBCELLS_PER_CELL;
constexpr int64_t CHUNK_SIZE_SUBCELLS = static_cast<int64_t>(shared::Chunk::SIDE_LENGTH)
    * SUBCELLS_PER_BLOCK;
constexpr int64_t WORLD_SIZE_SUBCELLS = static_cast<int64_t>(shared::WorldExtent::WIDTH)
    * SUBCELLS_PER_BLOCK;

[[nodiscard]]
int64_t floorDivide(int64_t const value, int64_t const divisor) noexcept
{
    int64_t const quotient = value / divisor;
    int64_t const remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

[[nodiscard]]
bool isValidPosition(shared::PlayerPosition const position) noexcept
{
    return position.x_subcell < shared::SUBCELLS_PER_CELL
        && position.y_subcell < shared::SUBCELLS_PER_CELL
        && position.z_subcell < shared::SUBCELLS_PER_CELL
        && shared::WorldBounds::isValidZ(position.z);
}

[[nodiscard]]
int64_t distanceToClosedInterval(
    int64_t const point,
    int64_t const lower,
    int64_t const upper
) noexcept
{
    if (point < lower) {
        return lower - point;
    }
    if (point > upper) {
        return point - upper;
    }
    return 0;
}

[[nodiscard]]
int64_t periodicDistanceToChunk(
    int64_t const center,
    int64_t const chunk_start,
    int64_t const chunk_end
) noexcept
{
    static constexpr std::array<int64_t, 3U> IMAGE_OFFSETS{
        -WORLD_SIZE_SUBCELLS,
        int64_t{0},
        WORLD_SIZE_SUBCELLS,
    };
    int64_t nearest = std::numeric_limits<int64_t>::max();
    for (int64_t const image : IMAGE_OFFSETS) {
        nearest = std::min(
            nearest,
            distanceToClosedInterval(center, chunk_start + image, chunk_end + image)
        );
    }
    return nearest;
}

[[nodiscard]]
bool intersectsClosedSphere(
    int64_t const center_x,
    int64_t const center_y,
    int64_t const center_z,
    shared::ChunkCoordinate const coordinate,
    int64_t const radius_squared
) noexcept
{
    int64_t const x_start = static_cast<int64_t>(coordinate.x) * CHUNK_SIZE_SUBCELLS;
    int64_t const y_start = static_cast<int64_t>(coordinate.y) * CHUNK_SIZE_SUBCELLS;
    int64_t const z_start = static_cast<int64_t>(coordinate.z) * CHUNK_SIZE_SUBCELLS;
    int64_t const x_distance = periodicDistanceToChunk(
        center_x,
        x_start,
        x_start + CHUNK_SIZE_SUBCELLS
    );
    int64_t const y_distance = periodicDistanceToChunk(
        center_y,
        y_start,
        y_start + CHUNK_SIZE_SUBCELLS
    );
    int64_t const z_distance = distanceToClosedInterval(
        center_z,
        z_start,
        z_start + CHUNK_SIZE_SUBCELLS
    );
    return x_distance * x_distance + y_distance * y_distance + z_distance * z_distance
        <= radius_squared;
}

[[nodiscard]]
bool isZyxLess(shared::ChunkCoordinate const left, shared::ChunkCoordinate const right) noexcept
{
    if (left.z != right.z) {
        return left.z < right.z;
    }
    if (left.y != right.y) {
        return left.y < right.y;
    }
    return left.x < right.x;
}

} // namespace

namespace shared {

namespace {

std::vector<ChunkCoordinate> chunkKeysWithinRadius(
    PlayerPosition const center,
    uint32_t const radius_blocks
)
{
    std::vector<ChunkCoordinate> result;
    if (!isValidPosition(center)) {
        return result;
    }

    int64_t const radius = static_cast<int64_t>(radius_blocks) * SUBCELLS_PER_BLOCK;
    int64_t const radius_squared = radius * radius;
    int64_t const center_x = static_cast<int64_t>(WorldBounds::wrapHorizontal(center.x))
        * SUBCELLS_PER_BLOCK + center.x_subcell;
    int64_t const center_y = static_cast<int64_t>(WorldBounds::wrapHorizontal(center.y))
        * SUBCELLS_PER_BLOCK + center.y_subcell;
    int64_t const center_z = static_cast<int64_t>(center.z) * SUBCELLS_PER_BLOCK + center.z_subcell;
    int64_t const first_x = floorDivide(center_x - radius, CHUNK_SIZE_SUBCELLS) - 1;
    int64_t const last_x = floorDivide(center_x + radius, CHUNK_SIZE_SUBCELLS);
    int64_t const first_y = floorDivide(center_y - radius, CHUNK_SIZE_SUBCELLS) - 1;
    int64_t const last_y = floorDivide(center_y + radius, CHUNK_SIZE_SUBCELLS);
    int64_t const first_z = std::max<int64_t>(
        0,
        floorDivide(center_z - radius, CHUNK_SIZE_SUBCELLS) - 1
    );
    int64_t const last_z = std::min<int64_t>(
        static_cast<int64_t>(WorldExtent::DEPTH / Chunk::SIDE_LENGTH) - 1,
        floorDivide(center_z + radius, CHUNK_SIZE_SUBCELLS)
    );

    uint64_t const x_candidates = static_cast<uint64_t>(last_x - first_x + 1);
    uint64_t const y_candidates = static_cast<uint64_t>(last_y - first_y + 1);
    uint64_t const z_candidates = last_z >= first_z
        ? static_cast<uint64_t>(last_z - first_z + 1)
        : 0U;
    uint64_t const candidate_count = x_candidates * y_candidates * z_candidates;
    result.reserve(static_cast<std::vector<ChunkCoordinate>::size_type>(candidate_count));

    for (int64_t chunk_z = first_z; chunk_z <= last_z; ++chunk_z) {
        for (int64_t raw_y = first_y; raw_y <= last_y; ++raw_y) {
            uint32_t const chunk_y = WorldBounds::wrapHorizontal(raw_y * Chunk::SIDE_LENGTH)
                / Chunk::SIDE_LENGTH;
            for (int64_t raw_x = first_x; raw_x <= last_x; ++raw_x) {
                uint32_t const chunk_x = WorldBounds::wrapHorizontal(raw_x * Chunk::SIDE_LENGTH)
                    / Chunk::SIDE_LENGTH;
                ChunkCoordinate const coordinate{
                    .x = static_cast<int32_t>(chunk_x),
                    .y = static_cast<int32_t>(chunk_y),
                    .z = static_cast<int32_t>(chunk_z),
                };
                if (intersectsClosedSphere(
                    center_x,
                    center_y,
                    center_z,
                    coordinate,
                    radius_squared
                )) {
                    result.push_back(coordinate);
                }
            }
        }
    }
    std::sort(result.begin(), result.end(), isZyxLess);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace

std::vector<ChunkCoordinate> requiredChunkKeys(
    PlayerPosition const center,
    uint32_t const radius_blocks
)
{
    if (radius_blocks > MAX_SPHERE_RADIUS_BLOCKS) {
        return {};
    }
    return chunkKeysWithinRadius(center, radius_blocks);
}


} // namespace shared
