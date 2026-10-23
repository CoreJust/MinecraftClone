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
    if (residentChunk(WorldBounds::chunkCoordinate(*normalized)) == nullptr
        && hasAuthoritativeSurface(WorldBounds::chunkCoordinate(*normalized))) {
        return static_cast<SparseWorld const&>(*this).blockAt(*normalized);
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
    return queryBlock(*normalized).block;
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
    auto const authority = m_authoritative_surfaces.find({.x = chunk_coordinate.x, .y = chunk_coordinate.y, .z = 0});
    if (authority != m_authoritative_surfaces.end()) {
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
        if (!matchesSurfaceMetadata(metadata, iterator->second.chunk, coordinate)) {
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
    if (!normalized) {
        return false;
    }
    Chunk* const chunk = ensureChunk(WorldBounds::chunkCoordinate(*normalized));
    if (!chunk) {
        return false;
    }
    bool const changed = chunk->setBlock(WorldBounds::blockCoordinate(*normalized), block);
    if (changed) {
        ChunkCoordinate const key = WorldBounds::chunkCoordinate(*normalized);
        if (m_authoritative_surfaces.contains({key.x, key.y, 0})) {
            m_authoritative_dense_overrides.insert(key);
            pinAuthoritativeSurface(key);
            auto const resident = m_resident.find(key);
            m_evictable_residents.erase({resident->second.last_access, key});
            auto& tile = m_authoritative_surfaces.at({key.x, key.y, 0});
            tile.revision = ++m_authoritative_surface_revision;
        }
        invalidateSurfaceMetadata(key);
    }
    return changed;
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
        result.push_back(coordinate);
    }
    std::ranges::sort(result, [](auto const left, auto const right) {
        return left.y != right.y ? left.y < right.y : left.x < right.x;
    });
    return result;
}

uint64_t SparseWorld::authoritativeDenseSectionMask(
    ChunkCoordinate const coordinate,
    AuthoritativeSurface const& tile
) const
{
    uint64_t result = 0U;
    uint64_t mask = tile.dense_mask;
    for (uint32_t section = 0U; mask != 0U; ++section, mask >>= 1U) {
        if ((mask & 1U) == 0U) {
            continue;
        }
        ChunkCoordinate const dense_key{coordinate.x, coordinate.y, static_cast<int32_t>(section)};
        auto const resident = m_resident.find(dense_key);
        if (resident != m_resident.end()
            && (m_authoritative_dense_overrides.contains(dense_key)
                || resident->second.authoritative_baseline_hash == 0U
                || resident->second.authoritative_baseline_hash != resident->second.chunk.contentIdentity().content_hash)) {
            result |= uint64_t{1} << section;
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
    result.dense_section_mask = authoritativeDenseSectionMask(key, tile);
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
    return m_authoritative_surfaces.contains(coordinate);
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
    uint64_t mask = authoritativeDenseSectionMask(coordinate, tile);
    for (uint32_t z = 0U; mask != 0U; ++z, mask >>= 1U) {
        if ((mask & 1U) != 0U) {
            auto const chunk = m_resident.find({coordinate.x, coordinate.y, static_cast<int32_t>(z)});
            if (chunk != m_resident.end()) {
                hash = (hash ^ z) * 1'099'511'628'211ULL;
                hash = (hash ^ chunk->second.chunk.contentIdentity().content_hash) * 1'099'511'628'211ULL;
            }
        }
    }
    return hash;
}

} // namespace shared
