#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace shared {

enum class Block : uint8_t {
    Air,
    Stone,
};

struct BlockCoordinate final {
    uint8_t x;
    uint8_t y;
    uint8_t z;

    constexpr bool operator==(BlockCoordinate const&) const noexcept = default;
};

struct ChunkCoordinate final {
    int32_t x;
    int32_t y;
    int32_t z;

    constexpr bool operator==(ChunkCoordinate const&) const noexcept = default;
};

struct ChunkContentIdentity final {
    uint64_t revision;
    uint64_t content_hash;

    constexpr bool operator==(ChunkContentIdentity const&) const noexcept = default;
};

class Chunk final {
public:
    static constexpr uint8_t SIDE_LENGTH = 16;
    static constexpr uint32_t BLOCK_COUNT = 4'096;
    static constexpr uint32_t FACE_BLOCK_COUNT = 256;
    static constexpr uint64_t DEFAULT_FIXTURE_SEED = 42;

    using Blocks = std::array<Block, BLOCK_COUNT>;

    explicit Chunk(ChunkCoordinate coordinate = {}) noexcept;
    Chunk(ChunkCoordinate coordinate, Blocks blocks);

    [[nodiscard]]
    static Chunk makeStoneFixture(
        ChunkCoordinate coordinate = {},
        uint64_t seed = DEFAULT_FIXTURE_SEED
    ) noexcept;

    [[nodiscard]]
    static int8_t sampleFixtureNoise(
        ChunkCoordinate chunk_coordinate,
        BlockCoordinate block_coordinate,
        uint64_t seed = DEFAULT_FIXTURE_SEED
    ) noexcept;

    [[nodiscard]]
    static constexpr bool isValid(BlockCoordinate const coordinate) noexcept
    {
        return coordinate.x < SIDE_LENGTH
            && coordinate.y < SIDE_LENGTH
            && coordinate.z < SIDE_LENGTH;
    }

    [[nodiscard]]
    constexpr ChunkCoordinate coordinate() const noexcept { return m_coordinate; }
    [[nodiscard]]
    std::optional<Block> blockAt(BlockCoordinate coordinate) const noexcept;
    [[nodiscard]]
    bool setBlock(BlockCoordinate coordinate, Block block) noexcept;
    [[nodiscard]]
    ChunkContentIdentity contentIdentity() const noexcept;
private:
    [[nodiscard]]
    static constexpr uint32_t blockIndex(BlockCoordinate const coordinate) noexcept {
        return static_cast<uint32_t>(coordinate.z) * SIDE_LENGTH * SIDE_LENGTH
            + static_cast<uint32_t>(coordinate.y) * SIDE_LENGTH
            + coordinate.x;
    }

    [[nodiscard]]
    static constexpr bool isSupported(Block const block) noexcept
    {
        return block == Block::Air || block == Block::Stone;
    }
private:
    ChunkCoordinate m_coordinate;
    Blocks m_blocks{};
    uint64_t m_revision = 0;
    uint64_t m_content_hash = 0;
};

static_assert(sizeof(Block) == 1);
static_assert(sizeof(Chunk::Blocks) == Chunk::BLOCK_COUNT);

} // namespace shared
