#include <shared/world/Chunk.hpp>

#include <stdexcept>
#include <utility>

namespace {

constexpr uint64_t FNV_OFFSET_BASIS = 14'695'981'039'346'656'037ULL;
constexpr uint64_t FNV_PRIME = 1'099'511'628'211ULL;
constexpr uint64_t MIX_INCREMENT = 0x9e37'79b9'7f4a'7c15ULL;
constexpr uint64_t MIX_MULTIPLIER_1 = 0xbf58'476d'1ce4'e5b9ULL;
constexpr uint64_t MIX_MULTIPLIER_2 = 0x94d0'49bb'1331'11ebULL;
constexpr uint64_t SAMPLE_RANGE = 7;
constexpr int8_t SAMPLE_MINIMUM = -3;

[[nodiscard]]
uint64_t contentHash(shared::Chunk::Blocks const& blocks) noexcept
{
    uint64_t hash = FNV_OFFSET_BASIS;
    for (shared::Block const block : blocks) {
        hash ^= static_cast<uint8_t>(block);
        hash *= FNV_PRIME;
    }
    return hash;
}

[[nodiscard]]
constexpr uint64_t mix(uint64_t value) noexcept
{
    value += MIX_INCREMENT;
    value = (value ^ (value >> 30U)) * MIX_MULTIPLIER_1;
    value = (value ^ (value >> 27U)) * MIX_MULTIPLIER_2;
    return value ^ (value >> 31U);
}

[[nodiscard]]
constexpr uint64_t mixCoordinate(uint64_t state, uint32_t coordinate) noexcept
{
    return mix(state ^ static_cast<uint64_t>(coordinate));
}

} // namespace

namespace shared {

Chunk::Chunk(ChunkCoordinate const coordinate) noexcept
    : m_coordinate(coordinate)
    , m_content_hash(contentHash(m_blocks))
{
}

Chunk::Chunk(ChunkCoordinate const coordinate, Blocks blocks)
    : m_coordinate(coordinate)
    , m_blocks(std::move(blocks))
    , m_content_hash(contentHash(m_blocks))
{
    for (Block const block : m_blocks) {
        if (!isSupported(block)) {
            throw std::invalid_argument{ "Chunk blocks must be Air or Stone" };
        }
    }
}

Chunk Chunk::makeStoneFixture(ChunkCoordinate const coordinate, uint64_t const seed) noexcept
{
    Blocks blocks;
    blocks.fill(Block::Air);
    for (uint8_t z = 0; z < SIDE_LENGTH; ++z) {
        for (uint8_t y = 0; y < SIDE_LENGTH; ++y) {
            for (uint8_t x = 0; x < SIDE_LENGTH; ++x) {
                BlockCoordinate const block_coordinate{ .x = x, .y = y, .z = z };
                int32_t const height = static_cast<int32_t>(z) - 6
                    + sampleFixtureNoise(coordinate, block_coordinate, seed);
                if (height < 0) {
                    blocks[blockIndex(block_coordinate)] = Block::Stone;
                }
            }
        }
    }
    return Chunk{ coordinate, std::move(blocks) };
}

int8_t Chunk::sampleFixtureNoise(
    ChunkCoordinate const chunk_coordinate,
    BlockCoordinate const block_coordinate,
    uint64_t const seed
) noexcept
{
    uint64_t state = mix(seed);
    state = mixCoordinate(state, static_cast<uint32_t>(chunk_coordinate.x));
    state = mixCoordinate(state, static_cast<uint32_t>(chunk_coordinate.y));
    state = mixCoordinate(state, static_cast<uint32_t>(chunk_coordinate.z));
    state = mixCoordinate(state, block_coordinate.x);
    state = mixCoordinate(state, block_coordinate.y);
    state = mixCoordinate(state, block_coordinate.z);
    int8_t const sample = static_cast<int8_t>(state % SAMPLE_RANGE);
    return static_cast<int8_t>(sample + SAMPLE_MINIMUM);
}

std::optional<Block> Chunk::blockAt(BlockCoordinate const coordinate) const noexcept
{
    if (!isValid(coordinate)) {
        return std::nullopt;
    }
    return m_blocks[blockIndex(coordinate)];
}

bool Chunk::setBlock(BlockCoordinate const coordinate, Block const block) noexcept
{
    if (!isValid(coordinate) || !isSupported(block)) {
        return false;
    }

    Block& existing = m_blocks[blockIndex(coordinate)];
    if (existing == block) {
        return false;
    }

    existing = block;
    ++m_revision;
    m_content_hash = contentHash(m_blocks);
    return true;
}

ChunkContentIdentity Chunk::contentIdentity() const noexcept
{
    return ChunkContentIdentity{
        .revision = m_revision,
        .content_hash = m_content_hash,
    };
}

} // namespace shared
