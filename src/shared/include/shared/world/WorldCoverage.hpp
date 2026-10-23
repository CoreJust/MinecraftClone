#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <cstdint>
#include <vector>

namespace shared {

inline constexpr uint32_t MAX_SPHERE_RADIUS_BLOCKS = 320U;
static_assert(MAX_SPHERE_RADIUS_BLOCKS == 20U * Chunk::SIDE_LENGTH);
inline constexpr uint32_t FULL_DETAIL_RADIUS_CHUNKS = 40U;
inline constexpr uint32_t FULL_DETAIL_RADIUS_BLOCKS = FULL_DETAIL_RADIUS_CHUNKS * Chunk::SIDE_LENGTH;
inline constexpr uint32_t ACTUAL_TERRAIN_RADIUS_BLOCKS = FULL_DETAIL_RADIUS_BLOCKS;
inline constexpr uint32_t AUTHORITATIVE_PREFETCH_RADIUS_CHUNKS = 48U;
inline constexpr uint32_t AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS =
    AUTHORITATIVE_PREFETCH_RADIUS_CHUNKS * Chunk::SIDE_LENGTH;
inline constexpr uint32_t MAX_AUTHORITATIVE_COLUMNS_PER_PLAYER = 97U * 97U;
inline constexpr uint32_t MAX_FOUR_PLAYER_AUTHORITATIVE_COLUMNS = MAX_AUTHORITATIVE_COLUMNS_PER_PLAYER * 4U;
inline constexpr uint32_t NEAR_TERRAIN_RADIUS_BLOCKS = 48U;
inline constexpr uint32_t FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS = 320U;
static_assert(FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS == MAX_SPHERE_RADIUS_BLOCKS);

// Unit cubes around candidate chunk centers fit inside radius 20+sqrt(3)<21.733 chunks.
// With pi<22/7, their enclosing ball has volume below 43,016 chunks.
inline constexpr uint32_t MAX_CHUNK_KEYS_PER_SPHERE_UPPER_BOUND = 43'016U;
// Radius-48 chunk centers fit inside 3+sqrt(3)<4.733 chunk units.
// With pi<22/7 the enclosing ball has volume below 445; reserve 512 keys.
inline constexpr uint32_t MAX_CHUNK_KEYS_PER_NEAR_SPHERE_UPPER_BOUND = 512U;
inline constexpr uint64_t MAX_FOUR_PLAYER_PROTECTED_CHUNK_KEYS =
    static_cast<uint64_t>(MAX_CHUNK_KEYS_PER_NEAR_SPHERE_UPPER_BOUND) * 4U;

inline constexpr uint32_t MATERIALIZED_PREFETCH_MARGIN_BLOCKS = 32U;
inline constexpr uint32_t MAX_MATERIALIZED_INTEREST_RADIUS_BLOCKS =
    NEAR_TERRAIN_RADIUS_BLOCKS + MATERIALIZED_PREFETCH_MARGIN_BLOCKS;
static_assert(MAX_MATERIALIZED_INTEREST_RADIUS_BLOCKS == 80U);

// Dense near-body storage is separate from the full authoritative horizontal columns.
inline constexpr uint32_t MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND = 1'024U;
inline constexpr uint64_t MAX_FOUR_PLAYER_MATERIALIZED_INTEREST_CHUNK_KEYS =
    static_cast<uint64_t>(MAX_CHUNK_KEYS_PER_MATERIALIZED_INTEREST_UPPER_BOUND) * 4U;

[[nodiscard]]
std::vector<ChunkCoordinate> requiredChunkKeys(PlayerPosition center, uint32_t radius_blocks);

[[nodiscard]]
std::vector<ChunkCoordinate> requiredTerrainColumns(
    PlayerPosition center, uint32_t radius_blocks = FULL_DETAIL_RADIUS_BLOCKS
);

[[nodiscard]]
std::vector<ChunkCoordinate> materializedInterestChunkKeys(PlayerPosition center);

} // namespace shared
