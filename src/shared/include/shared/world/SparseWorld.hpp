#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace shared {

struct WorldCoordinate final {
    int64_t x;
    int64_t y;
    int64_t z;

    constexpr bool operator==(WorldCoordinate const&) const noexcept = default;
};

struct WorldExtent final {
    static constexpr uint32_t WIDTH = 65'536U;
    static constexpr uint32_t DEPTH = 1'024U;
};

class WorldBounds final {
public:
    [[nodiscard]]
    static constexpr uint32_t wrapHorizontal(int64_t coordinate) noexcept
    {
        int64_t const remainder = coordinate % static_cast<int64_t>(WorldExtent::WIDTH);
        return static_cast<uint32_t>(remainder < 0 ? remainder + static_cast<int64_t>(WorldExtent::WIDTH) : remainder);
    }

    [[nodiscard]]
    static constexpr bool isValidZ(int64_t coordinate) noexcept
    {
        return coordinate >= 0 && coordinate < static_cast<int64_t>(WorldExtent::DEPTH);
    }

    [[nodiscard]]
    static constexpr std::optional<WorldCoordinate> normalize(WorldCoordinate coordinate) noexcept
    {
        if (!isValidZ(coordinate.z)) {
            return std::nullopt;
        }
        return WorldCoordinate{
            .x = static_cast<int64_t>(wrapHorizontal(coordinate.x)),
            .y = static_cast<int64_t>(wrapHorizontal(coordinate.y)),
            .z = coordinate.z,
        };
    }

    [[nodiscard]]
    static constexpr ChunkCoordinate chunkCoordinate(WorldCoordinate coordinate) noexcept
    {
        return ChunkCoordinate{
            .x = static_cast<int32_t>(coordinate.x / Chunk::SIDE_LENGTH),
            .y = static_cast<int32_t>(coordinate.y / Chunk::SIDE_LENGTH),
            .z = static_cast<int32_t>(coordinate.z / Chunk::SIDE_LENGTH),
        };
    }

    [[nodiscard]]
    static constexpr BlockCoordinate blockCoordinate(WorldCoordinate coordinate) noexcept
    {
        return BlockCoordinate{
            .x = static_cast<uint8_t>(coordinate.x % Chunk::SIDE_LENGTH),
            .y = static_cast<uint8_t>(coordinate.y % Chunk::SIDE_LENGTH),
            .z = static_cast<uint8_t>(coordinate.z % Chunk::SIDE_LENGTH),
        };
    }

    [[nodiscard]]
    static constexpr bool isValidChunk(ChunkCoordinate coordinate) noexcept
    {
        return coordinate.x >= 0
            && coordinate.x < static_cast<int32_t>(WorldExtent::WIDTH / Chunk::SIDE_LENGTH)
            && coordinate.y >= 0
            && coordinate.y < static_cast<int32_t>(WorldExtent::WIDTH / Chunk::SIDE_LENGTH)
            && coordinate.z >= 0
            && coordinate.z < static_cast<int32_t>(WorldExtent::DEPTH / Chunk::SIDE_LENGTH);
    }
};

struct SparseWorldOptions final {
    static constexpr uint64_t DEFAULT_MAX_RESIDENT_CHUNKS = 128U;

    uint64_t seed = WorldConfiguration::SEED;
    uint64_t max_resident_chunks = DEFAULT_MAX_RESIDENT_CHUNKS;
};

class SparseWorld final {
public:
    explicit SparseWorld(SparseWorldOptions options = {});

    [[nodiscard]]
    std::optional<Block> blockAt(WorldCoordinate coordinate);

    [[nodiscard]]
    std::optional<Block> blockAt(WorldCoordinate coordinate) const noexcept;

    [[nodiscard]]
    bool setBlock(WorldCoordinate coordinate, Block block);

    [[nodiscard]]
    Chunk const* residentChunk(ChunkCoordinate coordinate) const noexcept;

    [[nodiscard]]
    Chunk* ensureChunk(ChunkCoordinate coordinate);

    [[nodiscard]]
    bool evict(ChunkCoordinate coordinate) noexcept;

    [[nodiscard]]
    uint64_t residentChunkCount() const noexcept;

    [[nodiscard]]
    uint64_t maxResidentChunks() const noexcept;

    [[nodiscard]]
    SparseWorldOptions const& options() const noexcept;

private:
    struct ChunkCoordinateHash final {
        [[nodiscard]]
        uint64_t operator()(ChunkCoordinate coordinate) const noexcept;
    };

    struct ResidentChunk final {
        Chunk chunk;
        uint64_t last_access = 0U;
    };

    using ResidentChunks = std::unordered_map<ChunkCoordinate, ResidentChunk, ChunkCoordinateHash>;

    void touch(ResidentChunk& resident) noexcept;
    void evictIfFull() noexcept;

private:
    SparseWorldOptions m_options;
    TerrainGenerator m_generator;
    ResidentChunks m_resident;
    uint64_t m_access_clock = 0U;
};

} // namespace shared
