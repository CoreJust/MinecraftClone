#include <shared/world/SparseWorld.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>

namespace shared {

uint64_t SparseWorld::ChunkCoordinateHash::operator()(ChunkCoordinate const coordinate) const noexcept
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (int32_t const component : {coordinate.x, coordinate.y, coordinate.z}) {
        hash ^= static_cast<uint32_t>(component);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

SparseWorld::SparseWorld(SparseWorldOptions const options)
    : m_options(options)
{
    if (m_options.max_resident_chunks == 0U) {
        throw std::invalid_argument{"sparse world requires a positive resident chunk bound"};
    }
    m_resident.reserve(static_cast<ResidentChunks::size_type>(m_options.max_resident_chunks));
}

void SparseWorld::touch(ResidentChunk& resident) noexcept
{
    if (m_access_clock == std::numeric_limits<uint64_t>::max()) {
        for (auto& [coordinate, entry] : m_resident) {
            static_cast<void>(coordinate);
            entry.last_access /= 2U;
        }
        m_access_clock = 1U;
    } else {
        ++m_access_clock;
    }
    resident.last_access = m_access_clock;
}

void SparseWorld::evictIfFull() noexcept
{
    while (m_resident.size() >= m_options.max_resident_chunks) {
        auto victim = m_resident.end();
        for (auto candidate = m_resident.begin(); candidate != m_resident.end(); ++candidate) {
            if (victim == m_resident.end()
                || candidate->second.last_access < victim->second.last_access
                || (candidate->second.last_access == victim->second.last_access
                    && candidate->first.x < victim->first.x)
                || (candidate->second.last_access == victim->second.last_access
                    && candidate->first.x == victim->first.x
                    && candidate->first.y < victim->first.y)
                || (candidate->second.last_access == victim->second.last_access
                    && candidate->first.x == victim->first.x
                    && candidate->first.y == victim->first.y
                    && candidate->first.z < victim->first.z)) {
                victim = candidate;
            }
        }
        if (victim == m_resident.end()) {
            return;
        }
        m_resident.erase(victim);
    }
}

Chunk* SparseWorld::ensureChunk(ChunkCoordinate const coordinate)
{
    if (!WorldBounds::isValidChunk(coordinate)) {
        return nullptr;
    }
    if (auto const existing = m_resident.find(coordinate); existing != m_resident.end()) {
        touch(existing->second);
        return &existing->second.chunk;
    }
    evictIfFull();
    auto [iterator, inserted] = m_resident.emplace(
        coordinate,
        ResidentChunk{.chunk = m_generator.generateChunk(coordinate)}
    );
    if (!inserted) {
        return nullptr;
    }
    touch(iterator->second);
    return &iterator->second.chunk;
}

std::optional<Block> SparseWorld::blockAt(WorldCoordinate const coordinate)
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized) {
        return std::nullopt;
    }
    Chunk* const chunk = ensureChunk(WorldBounds::chunkCoordinate(*normalized));
    if (!chunk) {
        return std::nullopt;
    }
    return chunk->blockAt(WorldBounds::blockCoordinate(*normalized));
}

std::optional<Block> SparseWorld::blockAt(WorldCoordinate const coordinate) const noexcept
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized) {
        return std::nullopt;
    }
    Chunk const* const chunk = residentChunk(WorldBounds::chunkCoordinate(*normalized));
    if (!chunk) {
        return std::nullopt;
    }
    return chunk->blockAt(WorldBounds::blockCoordinate(*normalized));
}

bool SparseWorld::setBlock(WorldCoordinate const coordinate, Block const block)
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized) {
        return false;
    }
    Chunk* const chunk = ensureChunk(WorldBounds::chunkCoordinate(*normalized));
    if (!chunk) {
        return false;
    }
    return chunk->setBlock(WorldBounds::blockCoordinate(*normalized), block);
}

Chunk const* SparseWorld::residentChunk(ChunkCoordinate const coordinate) const noexcept
{
    auto const iterator = m_resident.find(coordinate);
    return iterator == m_resident.end() ? nullptr : &iterator->second.chunk;
}

bool SparseWorld::evict(ChunkCoordinate const coordinate) noexcept
{
    return m_resident.erase(coordinate) != 0U;
}

uint64_t SparseWorld::residentChunkCount() const noexcept
{
    return static_cast<uint64_t>(m_resident.size());
}

uint64_t SparseWorld::maxResidentChunks() const noexcept
{
    return m_options.max_resident_chunks;
}

SparseWorldOptions const& SparseWorld::options() const noexcept
{
    return m_options;
}

} // namespace shared
