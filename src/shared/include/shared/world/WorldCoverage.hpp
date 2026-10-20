#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <cstdint>
#include <vector>

namespace shared {

inline constexpr uint32_t MAX_SPHERE_RADIUS_BLOCKS = 320U;
static_assert(MAX_SPHERE_RADIUS_BLOCKS == 20U * Chunk::SIDE_LENGTH);

// Unit cubes around candidate chunk centers fit inside radius 20+sqrt(3)<21.733 chunks.
// With pi<22/7, their enclosing ball has volume below 43,016 chunks.
inline constexpr uint32_t MAX_CHUNK_KEYS_PER_SPHERE_UPPER_BOUND = 43'016U;
inline constexpr uint64_t MAX_FOUR_PLAYER_PROTECTED_CHUNK_KEYS = 172'064U;

[[nodiscard]]
std::vector<ChunkCoordinate> requiredChunkKeys(PlayerPosition center, uint32_t radius_blocks);

} // namespace shared
