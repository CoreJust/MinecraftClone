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
    m_previews.reserve(static_cast<PreviewChunks::size_type>(m_options.max_resident_chunks));
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
    while (m_resident.size() + m_previews.size() >= m_options.max_resident_chunks) {
        auto preview_victim = m_previews.end();
        for (auto candidate = m_previews.begin(); candidate != m_previews.end(); ++candidate) {
            if (preview_victim == m_previews.end()
                || candidate->first.x > preview_victim->first.x
                || (candidate->first.x == preview_victim->first.x
                    && candidate->first.y > preview_victim->first.y)
                || (candidate->first.x == preview_victim->first.x
                    && candidate->first.y == preview_victim->first.y
                    && candidate->first.z > preview_victim->first.z)) {
                preview_victim = candidate;
            }
        }
        if (preview_victim != m_previews.end()) {
            m_previews.erase(preview_victim);
            continue;
        }
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
        if (victim != m_resident.end()) {
            m_resident.erase(victim);
            continue;
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
    ChunkCoordinate const preview_coordinate{.x = coordinate.x, .y = coordinate.y, .z = 0};
    if (auto const existing = m_resident.find(coordinate); existing != m_resident.end()) {
        m_previews.erase(preview_coordinate);
        touch(existing->second);
        return &existing->second.chunk;
    }
    m_previews.erase(preview_coordinate);
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

SparseBlockQuery SparseWorld::queryBlock(WorldCoordinate const coordinate) const noexcept
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized) {
        return {};
    }
    ChunkCoordinate const chunk_coordinate = WorldBounds::chunkCoordinate(*normalized);
    if (Chunk const* const chunk = residentChunk(chunk_coordinate); chunk != nullptr) {
        return {
            .state = GenerationState::Materialized,
            .block = chunk->blockAt(WorldBounds::blockCoordinate(*normalized)),
        };
    }
    auto const preview = m_previews.find({.x = chunk_coordinate.x, .y = chunk_coordinate.y, .z = 0});
    if (preview != m_previews.end()
        && preview->second.revision == m_options.revision
        && preview->second.seed == m_options.seed) {
        return {
            .state = GenerationState::Preview,
        };
    }
    return {};
}

bool SparseWorld::publishPreview(
    ChunkCoordinate const coordinate,
    HeightTile tile,
    uint64_t const revision,
    uint64_t const seed
)
{
    if (!WorldBounds::isValidChunk(coordinate)
        || tile.coordinate.x != coordinate.x
        || tile.coordinate.y != coordinate.y
        || revision != m_options.revision
        || seed != m_options.seed
        || m_resident.contains(coordinate)) {
        return false;
    }
    ChunkCoordinate const preview_coordinate{.x = coordinate.x, .y = coordinate.y, .z = 0};
    if (m_previews.contains(preview_coordinate)) {
        m_previews.insert_or_assign(preview_coordinate, Preview{
            .tile = std::move(tile),
            .revision = revision,
            .seed = seed,
        });
        return true;
    }
    if (m_resident.size() + m_previews.size() >= m_options.max_resident_chunks) {
        evictIfFull();
    }
    if (m_resident.size() + m_previews.size() >= m_options.max_resident_chunks) {
        return false;
    }
    m_previews.emplace(preview_coordinate, Preview{
        .tile = std::move(tile),
        .revision = revision,
        .seed = seed,
    });
    return true;
}

bool SparseWorld::publishMaterializedChunk(
    Chunk chunk,
    uint64_t const revision,
    uint64_t const seed
)
{
    ChunkCoordinate const coordinate = chunk.coordinate();
    if (!WorldBounds::isValidChunk(coordinate)
        || revision != m_options.revision
        || seed != m_options.seed) {
        return false;
    }
    m_previews.erase({.x = coordinate.x, .y = coordinate.y, .z = 0});
    if (auto const existing = m_resident.find(coordinate); existing != m_resident.end()) {
        existing->second.chunk = std::move(chunk);
        touch(existing->second);
        return true;
    }
    evictIfFull();
    auto [iterator, inserted] = m_resident.emplace(
        coordinate,
        ResidentChunk{.chunk = std::move(chunk)}
    );
    if (!inserted) {
        return false;
    }
    touch(iterator->second);
    return true;
}

void SparseWorld::setWorldIdentity(uint64_t const revision, uint64_t const seed) noexcept
{
    if (revision == m_options.revision && seed == m_options.seed) {
        return;
    }
    m_options.revision = revision;
    m_options.seed = seed;
    m_resident.clear();
    m_previews.clear();
    m_access_clock = 0U;
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
    bool const removed_resident = m_resident.erase(coordinate) != 0U;
    bool const removed_preview = m_previews.erase({.x = coordinate.x, .y = coordinate.y, .z = 0}) != 0U;
    return removed_preview || removed_resident;
}

uint64_t SparseWorld::residentChunkCount() const noexcept
{
    return static_cast<uint64_t>(m_resident.size());
}

uint64_t SparseWorld::maxResidentChunks() const noexcept
{
    return m_options.max_resident_chunks;
}

uint64_t SparseWorld::previewCount() const noexcept
{
    return static_cast<uint64_t>(m_previews.size());
}

SparseWorldOptions const& SparseWorld::options() const noexcept
{
    return m_options;
}

} // namespace shared
