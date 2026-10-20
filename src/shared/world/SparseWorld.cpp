#include <shared/world/SparseWorld.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

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
    : SparseWorld(options, SparseWorldGenerationMode::Local)
{}

SparseWorld::SparseWorld(
    SparseWorldOptions const options,
    SparseWorldGenerationMode const generation_mode
)
    : m_options(options)
{
    if (m_options.max_resident_chunks == 0U) {
        throw std::invalid_argument{"sparse world requires a positive resident chunk bound"};
    }
    if (generation_mode == SparseWorldGenerationMode::Local) {
        m_generator.emplace();
    } else if (generation_mode != SparseWorldGenerationMode::MaterializedOnly) {
        throw std::invalid_argument{"sparse world generation mode is invalid"};
    }
    m_resident.reserve(static_cast<ResidentChunks::size_type>(m_options.max_resident_chunks));
    m_previews.reserve(static_cast<PreviewChunks::size_type>(m_options.max_resident_chunks));
}

SparseWorld::SparseWorld(
    SparseWorldOptions const options,
    std::shared_ptr<TerrainWorld const> terrain_world
)
    : m_options(options)
    , m_terrain_world(std::move(terrain_world))
{
    if (m_options.max_resident_chunks == 0U) {
        throw std::invalid_argument{ "sparse world requires a positive resident chunk bound" };
    }
    if (!m_terrain_world
        || m_terrain_world->identity().seed != m_options.seed
        || m_terrain_world->identity().revision != m_options.revision) {
        throw std::invalid_argument{ "sparse world terrain context must match its seed and revision" };
    }
    m_resident.reserve(static_cast<ResidentChunks::size_type>(m_options.max_resident_chunks));
    m_previews.reserve(static_cast<PreviewChunks::size_type>(m_options.max_resident_chunks));
}

bool SparseWorld::ResidentEvictionCandidateLess::operator()(
    ResidentEvictionCandidate const& left,
    ResidentEvictionCandidate const& right
) const noexcept
{
    if (left.last_access != right.last_access) {
        return left.last_access < right.last_access;
    }
    if (left.coordinate.x != right.coordinate.x) {
        return left.coordinate.x < right.coordinate.x;
    }
    if (left.coordinate.y != right.coordinate.y) {
        return left.coordinate.y < right.coordinate.y;
    }
    return left.coordinate.z < right.coordinate.z;
}

void SparseWorld::advanceAccessClock() noexcept
{
    if (m_access_clock == std::numeric_limits<uint64_t>::max()) {
        ResidentEvictionCandidates normalized_candidates;
        while (!m_evictable_residents.empty()) {
            auto candidate = m_evictable_residents.extract(m_evictable_residents.begin());
            candidate.value().last_access /= 2U;
            normalized_candidates.insert(std::move(candidate));
        }
        for (auto& [candidate_coordinate, entry] : m_resident) {
            static_cast<void>(candidate_coordinate);
            entry.last_access /= 2U;
        }
        m_evictable_residents.swap(normalized_candidates);
        m_access_clock = 1U;
    } else {
        ++m_access_clock;
    }
}

void SparseWorld::touch(ChunkCoordinate const coordinate, ResidentChunk& resident) noexcept
{
    advanceAccessClock();
    if (!m_protected_chunks.contains(coordinate)) {
        auto const candidate = m_evictable_residents.find({
            .last_access = resident.last_access,
            .coordinate = coordinate,
        });
        if (candidate != m_evictable_residents.end()) {
            auto node = m_evictable_residents.extract(candidate);
            node.value().last_access = m_access_clock;
            m_evictable_residents.insert(std::move(node));
        }
    }
    resident.last_access = m_access_clock;
}

void SparseWorld::indexResidentForEviction(
    ChunkCoordinate const coordinate,
    ResidentChunk const& resident
)
{
    if (!m_protected_chunks.contains(coordinate)) {
        m_evictable_residents.insert({
            .last_access = resident.last_access,
            .coordinate = coordinate,
        });
    }
}

bool SparseWorld::evictIfFull(std::optional<ChunkCoordinate>* const evicted_coordinate) noexcept
{
    if (evicted_coordinate != nullptr) {
        evicted_coordinate->reset();
    }
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
        if (!m_evictable_residents.empty()) {
            auto const victim = m_evictable_residents.begin();
            ChunkCoordinate const coordinate = victim->coordinate;
            if (evicted_coordinate != nullptr) {
                *evicted_coordinate = coordinate;
            }
            m_evictable_residents.erase(victim);
            m_resident.erase(coordinate);
            continue;
        }
        return false;
    }
    return true;
}

Chunk* SparseWorld::ensureChunk(
    ChunkCoordinate const coordinate,
    std::optional<ChunkCoordinate>* const evicted_coordinate
)
{
    if (evicted_coordinate != nullptr) {
        evicted_coordinate->reset();
    }
    if (!WorldBounds::isValidChunk(coordinate)) {
        return nullptr;
    }
    ChunkCoordinate const preview_coordinate{.x = coordinate.x, .y = coordinate.y, .z = 0};
    if (auto const existing = m_resident.find(coordinate); existing != m_resident.end()) {
        m_previews.erase(preview_coordinate);
        touch(coordinate, existing->second);
        return &existing->second.chunk;
    }
    if (m_terrain_world
        && (m_terrain_world->identity().seed != m_options.seed
            || m_terrain_world->identity().revision != m_options.revision)) {
        return nullptr;
    }
    if (!m_terrain_world && !m_generator) {
        return nullptr;
    }
    if (!evictIfFull(evicted_coordinate)) {
        return nullptr;
    }
    m_previews.erase(preview_coordinate);
    Chunk generated;
    if (m_terrain_world) {
        SurfaceTile const surface = m_terrain_world->sampleSurface({
            .x = coordinate.x,
            .y = coordinate.y,
            .z = 0,
        });
        generated = m_terrain_world->materializeChunk(surface, coordinate);
    } else if (m_generator) {
        generated = m_generator->generateChunk(coordinate);
    } else {
        return nullptr;
    }
    advanceAccessClock();
    auto [iterator, inserted] = m_resident.emplace(
        coordinate,
        ResidentChunk{.chunk = std::move(generated), .last_access = m_access_clock}
    );
    if (!inserted) {
        return nullptr;
    }
    try {
        indexResidentForEviction(coordinate, iterator->second);
    } catch (...) {
        m_resident.erase(iterator);
        throw;
    }
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
            .block = std::nullopt,
        };
    }
    return {};
}

bool SparseWorld::publishPreview(
    ChunkCoordinate const coordinate,
    HeightTile tile,
    uint64_t const revision,
    uint64_t const seed,
    std::optional<ChunkCoordinate>* const evicted_coordinate
)
{
    if (evicted_coordinate != nullptr) {
        evicted_coordinate->reset();
    }
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
            .tile = tile,
            .revision = revision,
            .seed = seed,
        });
        return true;
    }
    if (!evictIfFull(evicted_coordinate)) {
        return false;
    }
    m_previews.emplace(preview_coordinate, Preview{
        .tile = tile,
        .revision = revision,
        .seed = seed,
    });
    return true;
}

bool SparseWorld::publishMaterializedChunk(
    Chunk chunk,
    uint64_t const revision,
    uint64_t const seed,
    std::optional<ChunkCoordinate>* const evicted_coordinate
)
{
    if (evicted_coordinate != nullptr) {
        evicted_coordinate->reset();
    }
    ChunkCoordinate const coordinate = chunk.coordinate();
    if (!WorldBounds::isValidChunk(coordinate)
        || revision != m_options.revision
        || seed != m_options.seed) {
        return false;
    }
    if (auto const existing = m_resident.find(coordinate); existing != m_resident.end()) {
        m_previews.erase({.x = coordinate.x, .y = coordinate.y, .z = 0});
        touch(coordinate, existing->second);
        return true;
    }
    if (!evictIfFull(evicted_coordinate)) {
        return false;
    }
    m_previews.erase({.x = coordinate.x, .y = coordinate.y, .z = 0});
    advanceAccessClock();
    auto [iterator, inserted] = m_resident.emplace(
        coordinate,
        ResidentChunk{.chunk = std::move(chunk), .last_access = m_access_clock}
    );
    if (!inserted) {
        return false;
    }
    try {
        indexResidentForEviction(coordinate, iterator->second);
    } catch (...) {
        m_resident.erase(iterator);
        throw;
    }
    return true;
}

bool SparseWorld::setProtectedChunks(std::span<ChunkCoordinate const> const coordinates)
{
    ProtectedChunks replacement;
    replacement.reserve(coordinates.size());
    for (ChunkCoordinate const coordinate : coordinates) {
        if (!WorldBounds::isValidChunk(coordinate)) {
            return false;
        }
        replacement.insert(coordinate);
        if (replacement.size() > m_options.max_resident_chunks) {
            return false;
        }
    }
    bool const unchanged = replacement.size() == m_protected_chunks.size()
        && std::all_of(
            replacement.begin(),
            replacement.end(),
            [this](ChunkCoordinate const coordinate) {
                return m_protected_chunks.contains(coordinate);
            }
        );
    if (unchanged) {
        return true;
    }

    // Stage candidate allocations first so failure leaves both live indices unchanged.
    ResidentEvictionCandidates newly_evictable_residents;
    for (ChunkCoordinate const coordinate : m_protected_chunks) {
        if (replacement.contains(coordinate)) {
            continue;
        }
        auto const resident = m_resident.find(coordinate);
        if (resident != m_resident.end()) {
            newly_evictable_residents.insert({
                .last_access = resident->second.last_access,
                .coordinate = coordinate,
            });
        }
    }

    for (ChunkCoordinate const coordinate : replacement) {
        if (m_protected_chunks.contains(coordinate)) {
            continue;
        }
        auto const resident = m_resident.find(coordinate);
        if (resident != m_resident.end()) {
            m_evictable_residents.erase({
                .last_access = resident->second.last_access,
                .coordinate = coordinate,
            });
        }
    }
    m_evictable_residents.merge(newly_evictable_residents);
    m_protected_chunks.swap(replacement);
    return true;
}

bool SparseWorld::isChunkProtected(ChunkCoordinate const coordinate) const noexcept
{
    return m_protected_chunks.contains(coordinate);
}

uint64_t SparseWorld::protectedChunkCount() const noexcept
{
    return static_cast<uint64_t>(m_protected_chunks.size());
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
    m_protected_chunks.clear();
    m_evictable_residents.clear();
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
    if (isChunkProtected(coordinate)) {
        return false;
    }
    auto const resident = m_resident.find(coordinate);
    bool const removed_resident = resident != m_resident.end();
    if (removed_resident) {
        m_evictable_residents.erase({
            .last_access = resident->second.last_access,
            .coordinate = coordinate,
        });
        m_resident.erase(resident);
    }
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
