#include <shared/world/SparseWorld.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace shared {

namespace {

uint64_t retainedEditCapacityUnits(uint64_t const stored_cells) noexcept
{
    return stored_cells == 0U ? 1U : stored_cells;
}

} // namespace

Block AuthoritativeMaterialProfile::materialAt(
    uint16_t const z,
    uint32_t const block_sea_level
) const noexcept
{
    if (z == 0U) {
        return Block::Bedrock;
    }
    if (z > highest_solid_z) {
        return Block::Air;
    }
    if (highest_solid_z >= block_sea_level && z == highest_solid_z) {
        return top_material;
    }
    if (z >= ground_first_z && z < ground_end_z) {
        return ground_material;
    }
    return Block::Stone;
}

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
    m_terrain_identity = m_terrain_world->identity();
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

void SparseWorld::pinAuthoritativeSurface(ChunkCoordinate const coordinate) noexcept
{
    auto const tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
    if (tile != m_authoritative_surfaces.end()) {
        m_evictable_authoritative_surfaces.erase(tile->second.revision);
        tile->second.edited = true;
    }
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
    if (!isChunkProtected(coordinate)) {
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
    if (!isChunkProtected(coordinate)) {
        m_evictable_residents.insert({
            .last_access = resident.last_access,
            .coordinate = coordinate,
        });
    }
}

bool SparseWorld::evictIfFull(std::optional<ChunkCoordinate>* const evicted_coordinate)
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
            auto const resident = m_resident.find(coordinate);
            if (resident != m_resident.end() && resident->second.authoritative_baseline_hash != 0U
                && resident->second.authoritative_baseline_hash != resident->second.chunk.contentIdentity().content_hash) {
                m_authoritative_dense_overrides.insert(coordinate);
                pinAuthoritativeSurface(coordinate);
                m_evictable_residents.erase(victim);
                continue;
            }
            if (evicted_coordinate != nullptr) {
                *evicted_coordinate = coordinate;
            }
            m_evictable_residents.erase(victim);
            if (auto tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
                tile != m_authoritative_surfaces.end()) {
                tile->second.dense_mask &= ~(uint64_t{1} << static_cast<uint32_t>(coordinate.z));
            }
            m_resident.erase(coordinate);
            continue;
        }
        return false;
    }
    return true;
}

namespace {

Chunk makeAuthoritativeChunk(
    ChunkCoordinate const coordinate,
    std::array<AuthoritativeMaterialProfile, Chunk::FACE_BLOCK_COUNT> const& columns,
    uint32_t const block_sea_level
)
{
    Chunk::Blocks blocks{};
    for (uint32_t z = 0U; z < Chunk::SIDE_LENGTH; ++z) {
        for (uint32_t index = 0U; index < Chunk::FACE_BLOCK_COUNT; ++index) {
            blocks[z * Chunk::FACE_BLOCK_COUNT + index] = columns[index].materialAt(
                static_cast<uint16_t>(static_cast<uint32_t>(coordinate.z) * Chunk::SIDE_LENGTH + z),
                block_sea_level
            );
        }
    }
    return Chunk{coordinate, blocks};
}

Chunk makeTerrainChunk(TerrainWorld const& terrain_world, ChunkCoordinate const coordinate)
{
    SurfaceTile const surface = terrain_world.sampleSurface({
        .x = coordinate.x,
        .y = coordinate.y,
        .z = 0,
    });
    return terrain_world.materializeChunk(surface, coordinate);
}

uint32_t editCellIndex(BlockCoordinate const coordinate) noexcept
{
    return static_cast<uint32_t>(coordinate.z) * Chunk::FACE_BLOCK_COUNT
        + static_cast<uint32_t>(coordinate.y) * Chunk::SIDE_LENGTH
        + coordinate.x;
}

BlockCoordinate editCellCoordinate(uint32_t const index) noexcept
{
    return {
        .x = static_cast<uint8_t>(index % Chunk::SIDE_LENGTH),
        .y = static_cast<uint8_t>((index / Chunk::SIDE_LENGTH) % Chunk::SIDE_LENGTH),
        .z = static_cast<uint8_t>(index / Chunk::FACE_BLOCK_COUNT),
    };
}

bool chunksEqual(Chunk const& left, Chunk const& right) noexcept
{
    if (left.coordinate() != right.coordinate()) {
        return false;
    }
    for (uint8_t z = 0U; z < Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
                BlockCoordinate const coordinate{x, y, z};
                if (left.blockAt(coordinate) != right.blockAt(coordinate)) {
                    return false;
                }
            }
        }
    }
    return true;
}

template <typename ResidentMap, typename ChunkFactory, typename AdvanceAccess>
auto emplaceGeneratedChunk(
    ResidentMap& residents,
    ChunkCoordinate const coordinate,
    ChunkFactory&& make_chunk,
    AdvanceAccess&& advance_access
)
{
    Chunk chunk = make_chunk();
    uint64_t const last_access = advance_access();
    using ResidentChunk = typename ResidentMap::mapped_type;
    return residents.emplace(coordinate, ResidentChunk{
        .chunk = std::move(chunk),
        .last_access = last_access,
    });
}

} // namespace

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
    if (!editChunkRequirementSatisfied(coordinate)) {
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
    auto const authoritative = m_authoritative_surfaces.find(preview_coordinate);
    if (!m_terrain_world && !m_generator && authoritative == m_authoritative_surfaces.end()) {
        return nullptr;
    }
    if (!evictIfFull(evicted_coordinate)) {
        return nullptr;
    }
    m_previews.erase(preview_coordinate);
    auto [iterator, inserted] = emplaceGeneratedChunk(
        m_resident,
        coordinate,
        [this, coordinate, authoritative]() {
            if (authoritative != m_authoritative_surfaces.end()) {
                return makeAuthoritativeChunk(
                    coordinate,
                    authoritative->second.columns,
                    authoritative->second.block_sea_level
                );
            }
            if (m_terrain_world) {
                return makeTerrainChunk(*m_terrain_world, coordinate);
            }
            return m_generator->generateChunk(coordinate);
        },
        [this]() {
            advanceAccessClock();
            return m_access_clock;
        }
    );
    if (!inserted) {
        return nullptr;
    }
    applySparseEdits(iterator->second.chunk);
    if (auto tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
        tile != m_authoritative_surfaces.end()) {
        tile->second.dense_mask |= uint64_t{1} << static_cast<uint32_t>(coordinate.z);
        iterator->second.authoritative_baseline_hash = iterator->second.chunk.contentIdentity().content_hash;
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
    ChunkCoordinate const key = WorldBounds::chunkCoordinate(*normalized);
    uint32_t const edit_index = editCellIndex(WorldBounds::blockCoordinate(*normalized));
    auto const edits = m_sparse_edits.find(key);
    bool has_sparse_override = false;
    if (edits != m_sparse_edits.end()) {
        auto const cell = edits->second.cells.find(edit_index);
        has_sparse_override = cell != edits->second.cells.end() && cell->second.has_override;
    }
    if (residentChunk(key) == nullptr
        && (m_authoritative_surfaces.contains({key.x, key.y, 0}) || has_sparse_override)) {
        return static_cast<SparseWorld const&>(*this).blockAt(*normalized);
    }
    Chunk* const chunk = ensureChunk(key);
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
    return queryBlock(*normalized).block;
}

SparseBlockQuery SparseWorld::queryBlock(WorldCoordinate const coordinate) const noexcept
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized) {
        return {};
    }
    ChunkCoordinate const chunk_coordinate = WorldBounds::chunkCoordinate(*normalized);
    if (!editChunkRequirementSatisfied(chunk_coordinate)) {
        return {};
    }
    auto const edits = m_sparse_edits.find(chunk_coordinate);
    if (edits != m_sparse_edits.end()) {
        uint32_t const index = editCellIndex(WorldBounds::blockCoordinate(*normalized));
        auto const cell = edits->second.cells.find(index);
        if (cell != edits->second.cells.end() && cell->second.has_override) {
            return {.state = GenerationState::Materialized, .block = cell->second.material};
        }
    }
    if (Chunk const* const chunk = residentChunk(chunk_coordinate); chunk != nullptr) {
        return {
            .state = GenerationState::Materialized,
            .block = chunk->blockAt(WorldBounds::blockCoordinate(*normalized)),
        };
    }
    auto const authority = m_authoritative_surfaces.find({.x = chunk_coordinate.x, .y = chunk_coordinate.y, .z = 0});
    if (authority != m_authoritative_surfaces.end()) {
        if (hasIncompleteEditRequirements({chunk_coordinate.x, chunk_coordinate.y, 0})) {
            return {};
        }
        uint32_t const index = static_cast<uint32_t>(normalized->y % Chunk::SIDE_LENGTH) * Chunk::SIDE_LENGTH
            + static_cast<uint32_t>(normalized->x % Chunk::SIDE_LENGTH);
        return {.state = GenerationState::Materialized,
            .block = authority->second.columns[index].materialAt(
                static_cast<uint16_t>(normalized->z), authority->second.block_sea_level)};
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
    HeightTile const& tile,
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
        || seed != m_options.seed
        || !editChunkRequirementSatisfied(coordinate)) {
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
    applySparseEdits(chunk);
    m_previews.erase({.x = coordinate.x, .y = coordinate.y, .z = 0});
    advanceAccessClock();
    auto [iterator, inserted] = m_resident.emplace(
        coordinate,
        ResidentChunk{.chunk = chunk, .last_access = m_access_clock}
    );
    if (!inserted) {
        return false;
    }
    if (auto tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
        tile != m_authoritative_surfaces.end()) {
        tile->second.dense_mask |= uint64_t{1} << static_cast<uint32_t>(coordinate.z);
        iterator->second.authoritative_baseline_hash = iterator->second.chunk.contentIdentity().content_hash;
    }
    try {
        indexResidentForEviction(coordinate, iterator->second);
    } catch (...) {
        m_resident.erase(iterator);
        throw;
    }
    if (auto const tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
        tile != m_authoritative_surfaces.end()) {
        SurfaceMetadata metadata{.identity = tile->second.identity, .block_sea_level = tile->second.block_sea_level,
            .columns = tile->second.columns,
            .resident_chunks = {},
        };
        bool matches_expected = matchesSurfaceMetadata(metadata, iterator->second.chunk, coordinate);
        if (!matches_expected && m_sparse_edits.contains(coordinate)) {
            Chunk expected = makeAuthoritativeChunk(coordinate, metadata.columns, metadata.block_sea_level);
            applySparseEdits(expected);
            matches_expected = chunksEqual(expected, iterator->second.chunk);
        }
        if (!matches_expected) {
            m_authoritative_dense_overrides.insert(coordinate);
            pinAuthoritativeSurface(coordinate);
            m_evictable_residents.erase({iterator->second.last_access, coordinate});
            tile->second.revision = ++m_authoritative_surface_revision;
        }
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
                return m_protected_chunks.contains(coordinate) || m_authoritative_dense_overrides.contains(coordinate);
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
        if (resident != m_resident.end() && !m_authoritative_dense_overrides.contains(coordinate)) {
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
    return m_protected_chunks.contains(coordinate) || m_authoritative_dense_overrides.contains(coordinate);
}

uint64_t SparseWorld::protectedChunkCount() const noexcept
{
    return static_cast<uint64_t>(m_protected_chunks.size());
}

void SparseWorld::touchSurfaceMetadata(SurfaceMetadata& metadata) const noexcept
{
    if (m_surface_metadata_clock == std::numeric_limits<uint64_t>::max()) {
        for (auto& [coordinate, entry] : m_surface_metadata) {
            static_cast<void>(coordinate);
            entry.last_access /= 2U;
        }
        m_surface_metadata_clock = 0U;
    }
    ++m_surface_metadata_clock;
    metadata.last_access = m_surface_metadata_clock;
}

void SparseWorld::invalidateSurfaceMetadata(ChunkCoordinate const coordinate) const noexcept
{
    m_surface_metadata.erase({ .x = coordinate.x, .y = coordinate.y, .z = 0 });
}

bool SparseWorld::matchesSurfaceMetadata(
    SurfaceMetadata const& metadata,
    Chunk const& chunk,
    ChunkCoordinate const coordinate
) const noexcept
{
    if (chunk.coordinate() != coordinate || coordinate.z < 0
        || coordinate.z >= static_cast<int32_t>(metadata.chunk_identities.size())) {
        return false;
    }

    for (uint8_t y = 0U; y < Chunk::SIDE_LENGTH; ++y) {
        for (uint8_t x = 0U; x < Chunk::SIDE_LENGTH; ++x) {
            uint32_t const profile_index = static_cast<uint32_t>(y) * Chunk::SIDE_LENGTH + x;
            SurfaceMaterialProfile const& profile = metadata.columns[profile_index];
            for (uint8_t z = 0U; z < Chunk::SIDE_LENGTH; ++z) {
                uint32_t const world_z = static_cast<uint32_t>(coordinate.z) * Chunk::SIDE_LENGTH + z;
                auto const actual = chunk.blockAt({ .x = x, .y = y, .z = z });
                if (!actual.has_value()
                    || *actual != profile.materialAt(static_cast<uint16_t>(world_z), metadata.block_sea_level)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool SparseWorld::setTerrainIdentity(TerrainIdentity identity)
{
    if (!isValidTerrainIdentity(identity)
        || identity.seed != m_options.seed
        || identity.revision != m_options.revision
        || (m_terrain_world && identity != m_terrain_world->identity())) {
        return false;
    }
    if (m_terrain_identity && *m_terrain_identity == identity) {
        return true;
    }
    if (!m_authoritative_surfaces.empty()) {
        m_resident.clear();
        m_evictable_residents.clear();
    }
    m_authoritative_surfaces.clear();
    m_evictable_authoritative_surfaces.clear();
    m_sparse_edits.clear();
    m_retained_edit_capacity_units = 0U;
    m_edited_chunk_requirements.clear();
    m_protected_authoritative_columns.clear();
    m_authoritative_dense_overrides.clear();
    ++m_authoritative_surface_revision;
    m_terrain_identity = std::move(identity);
    m_surface_metadata.clear();
    m_surface_metadata_clock = 0U;
    return true;
}

bool SparseWorld::publishSurfaceMetadata(SurfaceTile const& tile)
{
    if (!m_terrain_identity
        || !isValidSurfaceTile(tile)
        || tile.identity != *m_terrain_identity
        || tile.identity.seed != m_options.seed
        || tile.identity.revision != m_options.revision) {
        return false;
    }

    SurfaceMetadata metadata{
        .identity = tile.identity,
        .block_sea_level = tile.block_sea_level,
        .resident_chunks = {},
    };
    for (uint32_t index = 0U; index < tile.columns.size(); ++index) {
        TerrainColumn const& column = tile.columns[index];
        metadata.columns[index] = SurfaceMaterialProfile{
            .highest_solid_z = column.highest_solid_z,
            .ground_first_z = column.ground_first_z,
            .ground_end_z = column.ground_end_z,
            .top_material = column.top_material,
            .ground_material = column.ground_material,
        };
    }

    ChunkCoordinate const coordinate{ .x = tile.coordinate.x, .y = tile.coordinate.y, .z = 0 };
    if (!m_surface_metadata.contains(coordinate)
        && m_surface_metadata.size() >= MAX_SURFACE_METADATA_TILES) {
        auto victim = m_surface_metadata.begin();
        for (auto candidate = m_surface_metadata.begin(); candidate != m_surface_metadata.end(); ++candidate) {
            if (candidate->second.last_access < victim->second.last_access
                || (candidate->second.last_access == victim->second.last_access
                    && candidate->first.x < victim->first.x)
                || (candidate->second.last_access == victim->second.last_access
                    && candidate->first.x == victim->first.x
                    && candidate->first.y < victim->first.y)) {
                victim = candidate;
            }
        }
        m_surface_metadata.erase(victim);
    }
    auto [iterator, inserted] = m_surface_metadata.insert_or_assign(coordinate, std::move(metadata));
    static_cast<void>(inserted);
    touchSurfaceMetadata(iterator->second);
    return true;
}

bool SparseWorld::removeSurfaceMetadata(ChunkCoordinate const coordinate)
{
    if (coordinate.z != 0 || coordinate.x < 0 || coordinate.y < 0
        || coordinate.x >= static_cast<int32_t>(WorldExtent::WIDTH / Chunk::SIDE_LENGTH)
        || coordinate.y >= static_cast<int32_t>(WorldExtent::WIDTH / Chunk::SIDE_LENGTH)) {
        return false;
    }
    return m_surface_metadata.erase(coordinate) != 0U;
}

std::optional<VerifiedSurfaceColumn> SparseWorld::verifiedSurfaceColumn(
    int64_t const world_x,
    int64_t const world_y
) const
{
    if (auto const authoritative = authoritativeColumn(world_x, world_y)) {
        return VerifiedSurfaceColumn{authoritative->highest_solid_z,
            authoritative->materialAt(authoritative->highest_solid_z)};
    }

    uint32_t const x = WorldBounds::wrapHorizontal(world_x);
    uint32_t const y = WorldBounds::wrapHorizontal(world_y);
    ChunkCoordinate const tile_coordinate{
        .x = static_cast<int32_t>(x / Chunk::SIDE_LENGTH),
        .y = static_cast<int32_t>(y / Chunk::SIDE_LENGTH),
        .z = 0,
    };
    if (hasIncompleteEditRequirements(tile_coordinate)) {
        return std::nullopt;
    }
    auto metadata_iterator = m_surface_metadata.find(tile_coordinate);
    if (metadata_iterator == m_surface_metadata.end()) {
        return std::nullopt;
    }

    SurfaceMetadata& metadata = metadata_iterator->second;
    if (!m_terrain_identity
        || metadata.identity != *m_terrain_identity
        || metadata.identity.seed != m_options.seed
        || metadata.identity.revision != m_options.revision) {
        m_surface_metadata.erase(metadata_iterator);
        return std::nullopt;
    }

    uint32_t const local_x = x % Chunk::SIDE_LENGTH;
    uint32_t const local_y = y % Chunk::SIDE_LENGTH;
    uint32_t const profile_index = local_y * Chunk::SIDE_LENGTH + local_x;
    SurfaceMaterialProfile const& profile = metadata.columns[profile_index];
    uint32_t const highest_solid_chunk_z = profile.highest_solid_z / Chunk::SIDE_LENGTH;
    ChunkCoordinate const highest_solid_coordinate{
        .x = tile_coordinate.x,
        .y = tile_coordinate.y,
        .z = static_cast<int32_t>(highest_solid_chunk_z),
    };
    if (!m_resident.contains(highest_solid_coordinate)) {
        return std::nullopt;
    }

    bool resident_set_matches = metadata.proof_ready;
    if (resident_set_matches) {
        for (uint32_t chunk_z = 0U; chunk_z < metadata.chunk_identities.size(); ++chunk_z) {
            ChunkCoordinate const coordinate{
                .x = tile_coordinate.x,
                .y = tile_coordinate.y,
                .z = static_cast<int32_t>(chunk_z),
            };
            auto const actual = m_resident.find(coordinate);
            bool const expected_resident = metadata.resident_chunks.test(chunk_z);
            if ((actual != m_resident.end()) != expected_resident) {
                resident_set_matches = false;
                break;
            }
            if (actual != m_resident.end()
                && actual->second.chunk.contentIdentity() != metadata.chunk_identities[chunk_z]) {
                m_surface_metadata.erase(metadata_iterator);
                return std::nullopt;
            }
        }
    }

    if (!resident_set_matches) {
        metadata.resident_chunks.reset();
        for (uint32_t chunk_z = 0U; chunk_z < metadata.chunk_identities.size(); ++chunk_z) {
            ChunkCoordinate const coordinate{
                .x = tile_coordinate.x,
                .y = tile_coordinate.y,
                .z = static_cast<int32_t>(chunk_z),
            };
            auto const actual = m_resident.find(coordinate);
            if (actual == m_resident.end()) {
                continue;
            }
            if (!matchesSurfaceMetadata(metadata, actual->second.chunk, coordinate)) {
                m_surface_metadata.erase(metadata_iterator);
                return std::nullopt;
            }
            metadata.resident_chunks.set(chunk_z);
            metadata.chunk_identities[chunk_z] = actual->second.chunk.contentIdentity();
        }
        metadata.proof_ready = true;
    }

    touchSurfaceMetadata(metadata);
    return VerifiedSurfaceColumn{
        .highest_solid_z = profile.highest_solid_z,
        .material_at_highest_solid = profile.materialAt(
            profile.highest_solid_z,
            metadata.block_sea_level
        ),
    };
}

uint64_t SparseWorld::surfaceMetadataTileCount() const noexcept
{
    return static_cast<uint64_t>(m_surface_metadata.size());
}

void SparseWorld::setWorldIdentity(uint64_t const revision, uint64_t const seed) noexcept
{
    if (revision == m_options.revision && seed == m_options.seed) {
        return;
    }
    m_options.revision = revision;
    m_options.seed = seed;
    m_authoritative_surfaces.clear();
    m_evictable_authoritative_surfaces.clear();
    m_sparse_edits.clear();
    m_retained_edit_capacity_units = 0U;
    m_edited_chunk_requirements.clear();
    m_protected_authoritative_columns.clear();
    m_authoritative_dense_overrides.clear();
    ++m_authoritative_surface_revision;
    m_resident.clear();
    m_previews.clear();
    m_protected_chunks.clear();
    m_evictable_residents.clear();
    m_terrain_identity.reset();
    m_surface_metadata.clear();
    m_surface_metadata_clock = 0U;
    m_access_clock = 0U;
}

bool SparseWorld::setBlock(WorldCoordinate const coordinate, Block const block)
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized || !isValidBlock(block)) {
        return false;
    }
    ChunkCoordinate const key = WorldBounds::chunkCoordinate(*normalized);
    BlockCoordinate const cell_coordinate = WorldBounds::blockCoordinate(*normalized);
    uint32_t const index = editCellIndex(cell_coordinate);
    SparseEditValue const* current_cell = nullptr;
    if (auto const current_edits = m_sparse_edits.find(key); current_edits != m_sparse_edits.end()) {
        if (auto const cell = current_edits->second.cells.find(index); cell != current_edits->second.cells.end()) {
            current_cell = &cell->second;
        }
    }
    bool const already_retained = current_cell != nullptr;
    if (already_retained && current_cell->material == block) {
        return false;
    }
    auto const current_edits = m_sparse_edits.find(key);
    bool const consumes_capacity = !already_retained
        && (current_edits == m_sparse_edits.end() || !current_edits->second.cells.empty());
    if (consumes_capacity && m_retained_edit_capacity_units >= m_options.max_retained_edit_cells) {
        return false;
    }
    Chunk* const chunk = ensureChunk(key);
    if (!chunk) {
        return false;
    }
    auto const previous = chunk->blockAt(cell_coordinate);
    if (!previous || *previous == block) {
        return false;
    }
    SparseChunkEdits& edits = m_sparse_edits[key];
    if (edits.revision == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    uint64_t const next_revision = edits.revision + 1U;
    SparseEditValue value{
        .material = block,
        .base_material = *previous,
        .cell_version = next_revision,
        .has_override = true,
    };
    if (auto const existing = edits.cells.find(index); existing != edits.cells.end()) {
        value.base_material = existing->second.base_material;
    }

    auto const resident = m_resident.find(key);
    uint64_t const before_hash = chunk->contentIdentity().content_hash;
    bool const had_known_baseline = resident != m_resident.end()
        && resident->second.authoritative_baseline_hash != 0U
        && resident->second.authoritative_baseline_hash == before_hash;
    edits.cells.insert_or_assign(index, value);
    if (!chunk->setBlock(cell_coordinate, block)) {
        return false;
    }
    if (consumes_capacity) {
        ++m_retained_edit_capacity_units;
    }
    edits.revision = next_revision;
    if (had_known_baseline) {
        resident->second.authoritative_baseline_hash = chunk->contentIdentity().content_hash;
    }

    auto const tile = m_authoritative_surfaces.find({key.x, key.y, 0});
    if (tile != m_authoritative_surfaces.end()) {
        pinAuthoritativeSurface(key);
        tile->second.revision = ++m_authoritative_surface_revision;
    }
    invalidateSurfaceMetadata(key);
    return true;
}

uint64_t SparseWorld::editChunkRevision(ChunkCoordinate const coordinate) const noexcept
{
    auto const edits = m_sparse_edits.find(coordinate);
    return edits == m_sparse_edits.end() ? 0U : edits->second.revision;
}

uint64_t SparseWorld::cellVersion(WorldCoordinate const coordinate) const noexcept
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized) {
        return 0U;
    }
    ChunkCoordinate const chunk_coordinate = WorldBounds::chunkCoordinate(*normalized);
    auto const edits = m_sparse_edits.find(chunk_coordinate);
    if (edits == m_sparse_edits.end()) {
        return 0U;
    }
    auto const cell = edits->second.cells.find(editCellIndex(WorldBounds::blockCoordinate(*normalized)));
    return cell == edits->second.cells.end() ? 0U : cell->second.cell_version;
}

std::vector<EditedChunkRevision> SparseWorld::editedChunks() const
{
    std::vector<EditedChunkRevision> result;
    result.reserve(m_sparse_edits.size());
    for (auto const& [coordinate, edits] : m_sparse_edits) {
        if (std::ranges::any_of(edits.cells, [](auto const& cell) { return cell.second.has_override; })) {
            result.push_back({coordinate, edits.revision});
        }
    }
    std::ranges::sort(result, [](EditedChunkRevision const& left, EditedChunkRevision const& right) {
        if (left.coordinate.x != right.coordinate.x) {
            return left.coordinate.x < right.coordinate.x;
        }
        if (left.coordinate.y != right.coordinate.y) {
            return left.coordinate.y < right.coordinate.y;
        }
        return left.coordinate.z < right.coordinate.z;
    });
    return result;
}

std::optional<SparseEditSnapshot> SparseWorld::editSnapshot(ChunkCoordinate const coordinate) const
{
    if (!WorldBounds::isValidChunk(coordinate)) {
        return std::nullopt;
    }
    auto const edits = m_sparse_edits.find(coordinate);
    if (edits == m_sparse_edits.end()) {
        return std::nullopt;
    }
    SparseEditSnapshot result{.coordinate = coordinate, .revision = edits->second.revision};
    result.edits.reserve(edits->second.cells.size());
    for (auto const& [index, cell] : edits->second.cells) {
        if (cell.has_override) {
            result.edits.push_back({editCellCoordinate(index), cell.material, cell.cell_version});
        }
    }
    std::ranges::sort(result.edits, [](SparseEditCell const& left, SparseEditCell const& right) {
        return editCellIndex(left.coordinate) < editCellIndex(right.coordinate);
    });
    return result;
}

std::optional<Chunk> SparseWorld::effectiveChunkSnapshot(ChunkCoordinate const coordinate) const
{
    if (!WorldBounds::isValidChunk(coordinate)) {
        return std::nullopt;
    }
    auto chunk = makeBaseChunk(coordinate);
    if (!chunk) {
        return std::nullopt;
    }
    applySparseEdits(*chunk);
    return chunk;
}

bool SparseWorld::installEditSnapshot(
    SparseEditSnapshot const& snapshot,
    Chunk const& effective_chunk
)
{
    ChunkCoordinate const coordinate = snapshot.coordinate;
    if (!WorldBounds::isValidChunk(coordinate)
        || effective_chunk.coordinate() != coordinate
        || snapshot.edits.size() > Chunk::BLOCK_COUNT) {
        return false;
    }
    auto const current = m_sparse_edits.find(coordinate);
    if (current != m_sparse_edits.end() && snapshot.revision <= current->second.revision) {
        return false;
    }

    uint64_t previous_capacity_units = 0U;
    uint64_t replacement_stored_cell_count = 0U;
    if (current != m_sparse_edits.end()) {
        previous_capacity_units = retainedEditCapacityUnits(current->second.cells.size());
        replacement_stored_cell_count = static_cast<uint64_t>(current->second.cells.size());
    }
    if (previous_capacity_units > m_retained_edit_capacity_units) {
        return false;
    }

    std::unordered_set<uint32_t> unique_cells;
    unique_cells.reserve(snapshot.edits.size());
    for (SparseEditCell const& edit : snapshot.edits) {
        if (!Chunk::isValid(edit.coordinate)
            || !isValidBlock(edit.material)
            || edit.cell_version == 0U
            || edit.cell_version > snapshot.revision) {
            return false;
        }
        uint32_t const index = editCellIndex(edit.coordinate);
        if (!unique_cells.insert(index).second
            || effective_chunk.blockAt(edit.coordinate) != edit.material) {
            return false;
        }
        if (current == m_sparse_edits.end() || !current->second.cells.contains(index)) {
            ++replacement_stored_cell_count;
        }
        if (current != m_sparse_edits.end()) {
            auto const old_cell = current->second.cells.find(index);
            if (old_cell != current->second.cells.end()
                && edit.cell_version < old_cell->second.cell_version) {
                return false;
            }
        }
    }
    uint64_t const retained_capacity_units = m_retained_edit_capacity_units - previous_capacity_units
        + retainedEditCapacityUnits(replacement_stored_cell_count);
    if (retained_capacity_units > m_options.max_retained_edit_cells) {
        return false;
    }

    auto base = makeBaseChunk(coordinate);
    if (base) {
        Chunk expected = *base;
        for (SparseEditCell const& edit : snapshot.edits) {
            static_cast<void>(expected.setBlock(edit.coordinate, edit.material));
        }
        if (!chunksEqual(expected, effective_chunk)) {
            return false;
        }
    }
    if (!m_resident.contains(coordinate) && !canAdmitResidentChunk()) {
        return false;
    }

    SparseChunkEdits replacement{.revision = snapshot.revision};
    replacement.cells.reserve(snapshot.edits.size() + (current == m_sparse_edits.end() ? 0U : current->second.cells.size()));
    for (SparseEditCell const& edit : snapshot.edits) {
        uint32_t const index = editCellIndex(edit.coordinate);
        Block base_material = effective_chunk.blockAt(edit.coordinate).value_or(Block::Air);
        if (base) {
            base_material = base->blockAt(edit.coordinate).value_or(base_material);
        } else if (current != m_sparse_edits.end()) {
            auto const old_cell = current->second.cells.find(index);
            if (old_cell != current->second.cells.end()) {
                base_material = old_cell->second.base_material;
            }
        }
        replacement.cells.emplace(index, SparseEditValue{
            .material = edit.material,
            .base_material = base_material,
            .cell_version = edit.cell_version,
            .has_override = true,
        });
    }
    if (current != m_sparse_edits.end()) {
        for (auto const& [index, old_cell] : current->second.cells) {
            if (replacement.cells.contains(index)) {
                continue;
            }
            Block const material = effective_chunk.blockAt(editCellCoordinate(index)).value_or(old_cell.material);
            replacement.cells.emplace(index, SparseEditValue{
                .material = material,
                .base_material = old_cell.base_material,
                .cell_version = std::max(old_cell.cell_version, snapshot.revision),
                .has_override = false,
            });
        }
    }

    auto [state, inserted_state] = m_sparse_edits.insert_or_assign(coordinate, std::move(replacement));
    static_cast<void>(inserted_state);
    m_retained_edit_capacity_units = retained_capacity_units;
    auto resident = m_resident.find(coordinate);
    if (resident == m_resident.end()) {
        if (!evictIfFull(nullptr)) {
            return false;
        }
        m_previews.erase({coordinate.x, coordinate.y, 0});
        advanceAccessClock();
        auto [inserted, success] = m_resident.emplace(coordinate, ResidentChunk{
            .chunk = effective_chunk,
            .last_access = m_access_clock,
        });
        if (!success) {
            return false;
        }
        resident = inserted;
        try {
            indexResidentForEviction(coordinate, resident->second);
        } catch (...) {
            m_resident.erase(resident);
            throw;
        }
    } else {
        resident->second.chunk = effective_chunk;
        touch(coordinate, resident->second);
    }
    resident->second.authoritative_baseline_hash = effective_chunk.contentIdentity().content_hash;
    m_authoritative_dense_overrides.erase(coordinate);
    if (auto tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
        tile != m_authoritative_surfaces.end()) {
        tile->second.dense_mask |= uint64_t{1} << static_cast<uint32_t>(coordinate.z);
        if (std::ranges::any_of(state->second.cells, [](auto const& cell) { return cell.second.has_override; })) {
            pinAuthoritativeSurface(coordinate);
        }
        if (snapshot.revision != 0U || !snapshot.edits.empty()) {
            tile->second.revision = ++m_authoritative_surface_revision;
        }
    }
    invalidateSurfaceMetadata(coordinate);
    return true;
}

bool SparseWorld::applyEditDelta(
    WorldCoordinate const coordinate,
    Block const material,
    uint64_t const from_revision,
    uint64_t const to_revision,
    uint64_t const cell_version
)
{
    auto const normalized = WorldBounds::normalize(coordinate);
    if (!normalized || !isValidBlock(material)
        || from_revision == std::numeric_limits<uint64_t>::max()
        || to_revision != from_revision + 1U
        || cell_version == 0U
        || cell_version > to_revision) {
        return false;
    }
    ChunkCoordinate const key = WorldBounds::chunkCoordinate(*normalized);
    auto const current_revision = m_sparse_edits.find(key);
    uint64_t const revision = current_revision == m_sparse_edits.end() ? 0U : current_revision->second.revision;
    if (revision != from_revision || cellVersion(*normalized) >= cell_version) {
        return false;
    }
    auto const previous = queryBlock(*normalized).block;
    if (previous && *previous == material) {
        return false;
    }

    uint32_t const index = editCellIndex(WorldBounds::blockCoordinate(*normalized));
    SparseEditValue const* current_cell = nullptr;
    if (current_revision != m_sparse_edits.end()) {
        if (auto const cell = current_revision->second.cells.find(index);
            cell != current_revision->second.cells.end()) {
            current_cell = &cell->second;
        }
    }
    bool const already_retained = current_cell != nullptr;
    bool const consumes_capacity = !already_retained
        && (current_revision == m_sparse_edits.end() || !current_revision->second.cells.empty());
    if (consumes_capacity && m_retained_edit_capacity_units >= m_options.max_retained_edit_cells) {
        return false;
    }

    SparseChunkEdits& edits = m_sparse_edits[key];
    SparseEditValue value{
        .material = material,
        .base_material = previous.value_or(Block::Air),
        .cell_version = cell_version,
        .has_override = true,
    };
    if (auto const old_cell = edits.cells.find(index); old_cell != edits.cells.end()) {
        value.base_material = old_cell->second.base_material;
    }
    edits.cells.insert_or_assign(index, value);
    if (consumes_capacity) {
        ++m_retained_edit_capacity_units;
    }
    edits.revision = to_revision;

    if (auto resident = m_resident.find(key); resident != m_resident.end()) {
        uint64_t const before_hash = resident->second.chunk.contentIdentity().content_hash;
        bool const known_baseline = resident->second.authoritative_baseline_hash != 0U
            && resident->second.authoritative_baseline_hash == before_hash;
        static_cast<void>(resident->second.chunk.setBlock(WorldBounds::blockCoordinate(*normalized), material));
        if (known_baseline) {
            resident->second.authoritative_baseline_hash = resident->second.chunk.contentIdentity().content_hash;
        }
    }
    if (auto tile = m_authoritative_surfaces.find({key.x, key.y, 0}); tile != m_authoritative_surfaces.end()) {
        pinAuthoritativeSurface(key);
        tile->second.revision = ++m_authoritative_surface_revision;
    }
    invalidateSurfaceMetadata(key);
    return true;
}

bool SparseWorld::setEditedChunkRequirements(
    ChunkCoordinate const coordinate,
    std::span<EditedChunkRevision const> const requirements
)
{
    if (!WorldBounds::isValidChunk(coordinate) || coordinate.z != 0
        || requirements.size() > WorldExtent::DEPTH / Chunk::SIDE_LENGTH) {
        return false;
    }
    EditedChunkRequirements values{};
    for (EditedChunkRevision const& requirement : requirements) {
        if (!WorldBounds::isValidChunk(requirement.coordinate)
            || requirement.coordinate.x != coordinate.x
            || requirement.coordinate.y != coordinate.y
            || requirement.revision == 0U
            || values[static_cast<size_t>(requirement.coordinate.z)] != 0U) {
            return false;
        }
        values[static_cast<size_t>(requirement.coordinate.z)] = requirement.revision;
    }
    if (requirements.empty()) {
        m_edited_chunk_requirements.erase(coordinate);
    } else {
        m_edited_chunk_requirements.insert_or_assign(coordinate, values);
    }
    invalidateSurfaceMetadata(coordinate);
    return true;
}

std::optional<Chunk> SparseWorld::makeBaseChunk(ChunkCoordinate const coordinate) const
{
    ChunkCoordinate const tile_coordinate{coordinate.x, coordinate.y, 0};
    if (auto const tile = m_authoritative_surfaces.find(tile_coordinate);
        tile != m_authoritative_surfaces.end()) {
        return makeAuthoritativeChunk(coordinate, tile->second.columns, tile->second.block_sea_level);
    }
    if (m_terrain_world
        && m_terrain_world->identity().seed == m_options.seed
        && m_terrain_world->identity().revision == m_options.revision) {
        return makeTerrainChunk(*m_terrain_world, coordinate);
    }
    if (m_generator) {
        return m_generator->generateChunk(coordinate);
    }
    auto const resident = m_resident.find(coordinate);
    if (resident == m_resident.end()) {
        return std::nullopt;
    }
    Chunk result = resident->second.chunk;
    if (auto const edits = m_sparse_edits.find(coordinate); edits != m_sparse_edits.end()) {
        for (auto const& [index, cell] : edits->second.cells) {
            static_cast<void>(result.setBlock(editCellCoordinate(index), cell.base_material));
        }
    }
    return result;
}

void SparseWorld::applySparseEdits(Chunk& chunk) const
{
    auto const edits = m_sparse_edits.find(chunk.coordinate());
    if (edits == m_sparse_edits.end()) {
        return;
    }
    for (auto const& [index, cell] : edits->second.cells) {
        if (cell.has_override) {
            static_cast<void>(chunk.setBlock(editCellCoordinate(index), cell.material));
        }
    }
}

bool SparseWorld::hasIncompleteEditRequirements(ChunkCoordinate coordinate) const noexcept
{
    coordinate.z = 0;
    auto const requirements = m_edited_chunk_requirements.find(coordinate);
    if (requirements == m_edited_chunk_requirements.end()) {
        return false;
    }
    for (uint32_t z = 0U; z < requirements->second.size(); ++z) {
        uint64_t const required_revision = requirements->second[z];
        if (required_revision != 0U
            && editChunkRevision({coordinate.x, coordinate.y, static_cast<int32_t>(z)}) < required_revision) {
            return true;
        }
    }
    return false;
}

bool SparseWorld::editChunkRequirementSatisfied(ChunkCoordinate const coordinate) const noexcept
{
    auto const requirements = m_edited_chunk_requirements.find({coordinate.x, coordinate.y, 0});
    if (requirements == m_edited_chunk_requirements.end()) {
        return true;
    }
    uint64_t const required_revision = requirements->second[static_cast<size_t>(coordinate.z)];
    return required_revision == 0U || editChunkRevision(coordinate) >= required_revision;
}

bool SparseWorld::canAdmitResidentChunk() const noexcept
{
    if (m_resident.size() + m_previews.size() < m_options.max_resident_chunks) {
        return true;
    }
    if (!m_previews.empty()) {
        return true;
    }
    return std::ranges::any_of(m_evictable_residents, [this](ResidentEvictionCandidate const& candidate) {
        auto const resident = m_resident.find(candidate.coordinate);
        return resident != m_resident.end()
            && (resident->second.authoritative_baseline_hash == 0U
                || resident->second.authoritative_baseline_hash
                    == resident->second.chunk.contentIdentity().content_hash);
    });
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
        if (auto tile = m_authoritative_surfaces.find({coordinate.x, coordinate.y, 0});
            tile != m_authoritative_surfaces.end()) {
            tile->second.dense_mask &= ~(uint64_t{1} << static_cast<uint32_t>(coordinate.z));
        }
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

Block AuthoritativeTerrainColumn::materialAt(uint16_t const z) const noexcept
{
    if (z >= WorldExtent::DEPTH) {
        return Block::Air;
    }
    if (dense_blocks) {
        return (*dense_blocks)[z];
    }
    return AuthoritativeMaterialProfile{highest_solid_z, ground_first_z, ground_end_z,
        top_material, ground_material}.materialAt(z, block_sea_level);
}

bool SparseWorld::publishAuthoritativeSurface(SurfaceTile const& tile)
{
    if (!isValidSurfaceTile(tile)) {
        return false;
    }
    AuthoritativeTerrainTile compact{
        .coordinate = tile.coordinate, .identity = tile.identity,
        .block_sea_level = tile.block_sea_level, .max_height = tile.max_height,
    };
    for (size_t index = 0U; index < tile.columns.size(); ++index) {
        auto const& column = tile.columns[index];
        compact.columns[index] = {column.highest_solid_z, column.ground_first_z, column.ground_end_z,
            column.top_material, column.ground_material};
    }
    return publishAuthoritativeSurface(compact);
}

bool SparseWorld::publishAuthoritativeSurface(AuthoritativeTerrainTile const& tile)
{
    if (!m_terrain_identity || tile.identity != *m_terrain_identity
        || tile.identity.seed != m_options.seed || tile.identity.revision != m_options.revision
        || !WorldBounds::isValidChunk(tile.coordinate) || tile.coordinate.z != 0
        || tile.max_height >= WorldExtent::DEPTH || tile.block_sea_level > tile.max_height
        || m_options.max_authoritative_columns == 0U) {
        return false;
    }
    for (auto const& column : tile.columns) {
        if (column.highest_solid_z > tile.max_height || column.ground_first_z > column.ground_end_z
            || column.ground_end_z > WorldExtent::DEPTH
            || (column.ground_first_z != column.ground_end_z
                && column.ground_end_z > static_cast<uint32_t>(column.highest_solid_z) + 1U)
            || !isSolidBlock(column.top_material) || !isSolidBlock(column.ground_material)) {
            return false;
        }
    }
    if (auto const existing = m_authoritative_surfaces.find(tile.coordinate);
        existing != m_authoritative_surfaces.end()) {
        return existing->second.block_sea_level == tile.block_sea_level
            && existing->second.max_height == tile.max_height && existing->second.columns == tile.columns;
    }
    if (!m_authoritative_surfaces.contains(tile.coordinate)
        && m_authoritative_surfaces.size() >= m_options.max_authoritative_columns) {
        if (m_evictable_authoritative_surfaces.empty()) {
            return false;
        }
        auto const victim = m_evictable_authoritative_surfaces.begin();
        m_authoritative_surfaces.erase(victim->second);
        m_evictable_authoritative_surfaces.erase(victim);
    }
    ++m_authoritative_surface_revision;
    m_authoritative_surfaces.insert_or_assign(tile.coordinate, AuthoritativeSurface{
        .identity = tile.identity, .block_sea_level = tile.block_sea_level, .max_height = tile.max_height,
        .columns = tile.columns, .revision = m_authoritative_surface_revision,
    });
    auto& published = m_authoritative_surfaces.at(tile.coordinate);
    if (!m_protected_authoritative_columns.contains(tile.coordinate)) {
        m_evictable_authoritative_surfaces.emplace(published.revision, tile.coordinate);
    }
    for (uint32_t z = 0U; z < WorldExtent::DEPTH / Chunk::SIDE_LENGTH; ++z) {
        auto const resident = m_resident.find({tile.coordinate.x, tile.coordinate.y, static_cast<int32_t>(z)});
        if (resident != m_resident.end()) {
            published.dense_mask |= uint64_t{1} << z;
        }
    }
    return true;
}

std::optional<AuthoritativeTerrainTile> SparseWorld::authoritativeTile(ChunkCoordinate coordinate) const
{
    coordinate.z = 0;
    auto const found = m_authoritative_surfaces.find(coordinate);
    if (found == m_authoritative_surfaces.end()) {
        return std::nullopt;
    }
    auto const& tile = found->second;
    return AuthoritativeTerrainTile{coordinate, tile.identity, tile.block_sea_level, tile.max_height,
        tile.columns, tile.revision};
}

bool SparseWorld::hasAuthoritativeChunk(ChunkCoordinate const coordinate) const noexcept
{
    return WorldBounds::isValidChunk(coordinate)
        && editChunkRequirementSatisfied(coordinate)
        && (m_resident.contains(coordinate) || m_authoritative_surfaces.contains({coordinate.x, coordinate.y, 0}));
}

bool SparseWorld::setProtectedAuthoritativeColumns(std::span<ChunkCoordinate const> const coordinates)
{
    ProtectedChunks replacement;
    replacement.reserve(coordinates.size());
    for (auto const coordinate : coordinates) {
        if (!WorldBounds::isValidChunk(coordinate) || coordinate.z != 0) {
            return false;
        }
        replacement.insert(coordinate);
    }
    if (replacement.size() > m_options.max_authoritative_columns) {
        return false;
    }
    for (auto const coordinate : replacement) {
        if (!m_protected_authoritative_columns.contains(coordinate)) {
            auto const tile = m_authoritative_surfaces.find(coordinate);
            if (tile != m_authoritative_surfaces.end()) {
                m_evictable_authoritative_surfaces.erase(tile->second.revision);
            }
        }
    }
    for (auto const coordinate : m_protected_authoritative_columns) {
        if (!replacement.contains(coordinate)) {
            auto const tile = m_authoritative_surfaces.find(coordinate);
            if (tile != m_authoritative_surfaces.end() && !tile->second.edited) {
                m_evictable_authoritative_surfaces.emplace(tile->second.revision, coordinate);
            }
        }
    }
    m_protected_authoritative_columns.swap(replacement);
    return true;
}

bool SparseWorld::removeAuthoritativeSurface(ChunkCoordinate coordinate) noexcept
{
    coordinate.z = 0;
    auto const tile = m_authoritative_surfaces.find(coordinate);
    if (tile == m_authoritative_surfaces.end() || m_protected_authoritative_columns.contains(coordinate)
        || tile->second.edited) {
        return false;
    }
    m_evictable_authoritative_surfaces.erase(tile->second.revision);
    m_authoritative_surfaces.erase(tile);
    ++m_authoritative_surface_revision;
    return true;
}

std::vector<ChunkCoordinate> SparseWorld::availableAuthoritativeColumns() const
{
    std::vector<ChunkCoordinate> result;
    result.reserve(m_authoritative_surfaces.size());
    for (auto const& [coordinate, tile] : m_authoritative_surfaces) {
        static_cast<void>(tile);
        if (!hasIncompleteEditRequirements(coordinate)) {
            result.push_back(coordinate);
        }
    }
    std::ranges::sort(result, [](auto const left, auto const right) {
        return left.y != right.y ? left.y < right.y : left.x < right.x;
    });
    return result;
}

uint64_t SparseWorld::authoritativeDenseSectionMask(
    ChunkCoordinate const coordinate,
    AuthoritativeSurface const& tile,
    uint8_t const local_x,
    uint8_t const local_y
) const
{
    uint64_t result = 0U;
    for (uint32_t section = 0U; section < WorldExtent::DEPTH / Chunk::SIDE_LENGTH; ++section) {
        ChunkCoordinate const dense_key{coordinate.x, coordinate.y, static_cast<int32_t>(section)};
        auto const edits = m_sparse_edits.find(dense_key);
        if (edits != m_sparse_edits.end()) {
            for (uint8_t local_z = 0U; local_z < Chunk::SIDE_LENGTH; ++local_z) {
                auto const cell = edits->second.cells.find(editCellIndex({local_x, local_y, local_z}));
                if (cell != edits->second.cells.end() && cell->second.has_override) {
                    result |= uint64_t{1} << section;
                    break;
                }
            }
        }
        if ((tile.dense_mask & (uint64_t{1} << section)) == 0U) {
            continue;
        }
        auto const resident = m_resident.find(dense_key);
        if (resident == m_resident.end()
            || (resident->second.authoritative_baseline_hash != 0U
                && resident->second.authoritative_baseline_hash == resident->second.chunk.contentIdentity().content_hash)) {
            continue;
        }
        for (uint8_t local_z = 0U; local_z < Chunk::SIDE_LENGTH; ++local_z) {
            uint32_t const world_z = section * Chunk::SIDE_LENGTH + local_z;
            Block expected = tile.columns[static_cast<uint32_t>(local_y) * Chunk::SIDE_LENGTH + local_x]
                .materialAt(static_cast<uint16_t>(world_z), tile.block_sea_level);
            if (edits != m_sparse_edits.end()) {
                auto const cell = edits->second.cells.find(editCellIndex({local_x, local_y, local_z}));
                if (cell != edits->second.cells.end() && cell->second.has_override) {
                    expected = cell->second.material;
                }
            }
            if (resident->second.chunk.blockAt({local_x, local_y, local_z}) != expected) {
                result |= uint64_t{1} << section;
                break;
            }
        }
    }
    return result;
}

std::optional<AuthoritativeTerrainColumn> SparseWorld::authoritativeColumn(int64_t const x, int64_t const y) const
{
    uint32_t const normalized_x = WorldBounds::wrapHorizontal(x);
    uint32_t const normalized_y = WorldBounds::wrapHorizontal(y);
    ChunkCoordinate const key{static_cast<int32_t>(normalized_x / Chunk::SIDE_LENGTH),
        static_cast<int32_t>(normalized_y / Chunk::SIDE_LENGTH), 0};
    if (hasIncompleteEditRequirements(key)) {
        return std::nullopt;
    }
    auto const found = m_authoritative_surfaces.find(key);
    if (found == m_authoritative_surfaces.end()) {
        return std::nullopt;
    }
    auto const& tile = found->second;
    auto const& profile = tile.columns[normalized_y % Chunk::SIDE_LENGTH * Chunk::SIDE_LENGTH
        + normalized_x % Chunk::SIDE_LENGTH];
    AuthoritativeTerrainColumn result{
        .highest_solid_z = profile.highest_solid_z, .base_highest_solid_z = profile.highest_solid_z,
        .ground_first_z = profile.ground_first_z,
        .ground_end_z = profile.ground_end_z, .top_material = profile.top_material,
        .ground_material = profile.ground_material, .block_sea_level = tile.block_sea_level,
        .identity = tile.identity, .revision = tile.revision,
    };
    uint8_t const local_x = static_cast<uint8_t>(normalized_x % Chunk::SIDE_LENGTH);
    uint8_t const local_y = static_cast<uint8_t>(normalized_y % Chunk::SIDE_LENGTH);
    result.dense_section_mask = authoritativeDenseSectionMask(key, tile, local_x, local_y);
    if (result.dense_section_mask != 0U) {
        auto blocks = std::make_shared<std::array<Block, WorldExtent::DEPTH>>();
        result.highest_solid_z = 0U;
        for (uint16_t z = 0U; z < WorldExtent::DEPTH; ++z) {
            auto const block = queryBlock({normalized_x, normalized_y, z}).block;
            if (!block) {
                return std::nullopt;
            }
            (*blocks)[z] = *block;
            if (isSolidBlock((*blocks)[z])) {
                result.highest_solid_z = z;
            }
        }
        result.dense_blocks = std::move(blocks);
    }
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    auto const mix = [&hash](uint64_t value) {
        for (uint32_t byte = 0U; byte < 8U; ++byte) {
            hash = (hash ^ (value & 255U)) * 1'099'511'628'211ULL;
            value >>= 8U;
        }
    };
    mix(result.highest_solid_z);
    mix(result.ground_first_z);
    mix(result.ground_end_z);
    mix(static_cast<uint8_t>(result.top_material));
    mix(static_cast<uint8_t>(result.ground_material));
    mix(result.block_sea_level);
    if (result.dense_blocks) {
        for (Block const block : *result.dense_blocks) {
            mix(static_cast<uint8_t>(block));
        }
    }
    result.content_hash = hash;
    return result;
}

bool SparseWorld::hasAuthoritativeSurface(ChunkCoordinate coordinate) const noexcept
{
    coordinate.z = 0;
    return m_authoritative_surfaces.contains(coordinate) && !hasIncompleteEditRequirements(coordinate);
}

std::optional<AuthoritativeTileIdentity> SparseWorld::authoritativeTileIdentity(ChunkCoordinate coordinate) const
{
    coordinate.z = 0;
    auto const found = m_authoritative_surfaces.find(coordinate);
    if (found == m_authoritative_surfaces.end()) {
        return std::nullopt;
    }
    return AuthoritativeTileIdentity{
        found->second.identity,
        found->second.revision,
        authoritativeTileContentHash(coordinate, found->second)
    };
}

bool SparseWorld::matchesAuthoritativeTileIdentity(
    ChunkCoordinate coordinate,
    TerrainIdentity const& identity,
    uint64_t revision,
    uint64_t content_hash
) const
{
    coordinate.z = 0;
    auto const found = m_authoritative_surfaces.find(coordinate);
    return found != m_authoritative_surfaces.end() && found->second.identity == identity
        && found->second.revision == revision
        && authoritativeTileContentHash(coordinate, found->second) == content_hash;
}

uint64_t SparseWorld::authoritativeTileContentHash(
    ChunkCoordinate coordinate, AuthoritativeSurface const& tile
) const
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    uint64_t dense_mask = tile.dense_mask;
    for (uint32_t z = 0U; dense_mask != 0U; ++z, dense_mask >>= 1U) {
        if ((dense_mask & 1U) == 0U) {
            continue;
        }
        auto const chunk = m_resident.find({coordinate.x, coordinate.y, static_cast<int32_t>(z)});
        if (chunk != m_resident.end()
            && (chunk->second.authoritative_baseline_hash == 0U
                || chunk->second.authoritative_baseline_hash != chunk->second.chunk.contentIdentity().content_hash)) {
            hash = (hash ^ z) * 1'099'511'628'211ULL;
            hash = (hash ^ chunk->second.chunk.contentIdentity().content_hash) * 1'099'511'628'211ULL;
        }
    }

    std::vector<ChunkCoordinate> edit_coordinates;
    for (auto const& [edit_coordinate, edits] : m_sparse_edits) {
        if (edit_coordinate.x == coordinate.x && edit_coordinate.y == coordinate.y && !edits.cells.empty()) {
            edit_coordinates.push_back(edit_coordinate);
        }
    }
    std::ranges::sort(edit_coordinates, [](ChunkCoordinate const left, ChunkCoordinate const right) {
        return left.z < right.z;
    });
    for (ChunkCoordinate const edit_coordinate : edit_coordinates) {
        auto const& edits = m_sparse_edits.at(edit_coordinate);
        hash = (hash ^ static_cast<uint32_t>(edit_coordinate.z)) * 1'099'511'628'211ULL;
        hash = (hash ^ edits.revision) * 1'099'511'628'211ULL;
        std::vector<uint32_t> indices;
        indices.reserve(edits.cells.size());
        for (auto const& [index, cell] : edits.cells) {
            static_cast<void>(cell);
            indices.push_back(index);
        }
        std::ranges::sort(indices);
        for (uint32_t const index : indices) {
            SparseEditValue const& cell = edits.cells.at(index);
            hash = (hash ^ index) * 1'099'511'628'211ULL;
            hash = (hash ^ static_cast<uint8_t>(cell.material)) * 1'099'511'628'211ULL;
            hash = (hash ^ cell.cell_version) * 1'099'511'628'211ULL;
            hash = (hash ^ static_cast<uint64_t>(cell.has_override)) * 1'099'511'628'211ULL;
        }
    }
    return hash;
}

} // namespace shared
