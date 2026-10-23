#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    uint64_t revision = 1U;
    uint64_t max_authoritative_columns = MAX_FOUR_PLAYER_AUTHORITATIVE_COLUMNS;
};

inline constexpr uint32_t MAX_SURFACE_METADATA_TILES = 1'024U;

enum class SparseWorldGenerationMode : uint8_t {
    Local,
    MaterializedOnly,
};

struct SparseBlockQuery final {
    GenerationState state = GenerationState::Unknown;
    std::optional<Block> block;
};

struct VerifiedSurfaceColumn final {
    uint16_t highest_solid_z = 0U;
    Block material_at_highest_solid = Block::Air;
};

struct AuthoritativeMaterialProfile final {
    uint16_t highest_solid_z = 0U;
    uint16_t ground_first_z = 0U;
    uint16_t ground_end_z = 0U;
    Block top_material = Block::Air;
    Block ground_material = Block::Air;
    [[nodiscard]] Block materialAt(uint16_t z, uint32_t block_sea_level) const noexcept;
    bool operator==(AuthoritativeMaterialProfile const&) const = default;
};

struct AuthoritativeTerrainTile final {
    ChunkCoordinate coordinate{};
    TerrainIdentity identity;
    uint32_t block_sea_level = 0U;
    uint32_t max_height = WorldExtent::DEPTH - 1U;
    std::array<AuthoritativeMaterialProfile, Chunk::FACE_BLOCK_COUNT> columns{};
    uint64_t revision = 0U;
};

struct AuthoritativeTileIdentity final {
    TerrainIdentity identity;
    uint64_t revision = 0U;
    uint64_t content_hash = 0U;
};

struct AuthoritativeTerrainColumn final {
    uint16_t highest_solid_z = 0U;
    uint16_t base_highest_solid_z = 0U;
    uint16_t ground_first_z = 0U;
    uint16_t ground_end_z = 0U;
    Block top_material = Block::Air;
    Block ground_material = Block::Air;
    uint32_t block_sea_level = 0U;
    TerrainIdentity identity;
    uint64_t revision = 0U;
    uint64_t content_hash = 0U;
    uint64_t dense_section_mask = 0U;
    std::shared_ptr<std::array<Block, WorldExtent::DEPTH> const> dense_blocks = nullptr;

    [[nodiscard]] Block materialAt(uint16_t z) const noexcept;
};

class SparseWorld final {
public:
    explicit SparseWorld(SparseWorldOptions options = {});
    SparseWorld(SparseWorldOptions options, SparseWorldGenerationMode generation_mode);
    SparseWorld(SparseWorldOptions options, std::shared_ptr<TerrainWorld const> terrain_world);

    [[nodiscard]]
    std::optional<Block> blockAt(WorldCoordinate coordinate);

    [[nodiscard]]
    std::optional<Block> blockAt(WorldCoordinate coordinate) const noexcept;

    [[nodiscard]]
    SparseBlockQuery queryBlock(WorldCoordinate coordinate) const noexcept;

    [[nodiscard]]
    bool publishPreview(
        ChunkCoordinate coordinate,
        HeightTile const& tile,
        uint64_t revision,
        uint64_t seed,
        std::optional<ChunkCoordinate>* evicted_coordinate = nullptr
    );

    [[nodiscard]]
    bool publishMaterializedChunk(
        Chunk chunk,
        uint64_t revision,
        uint64_t seed,
        std::optional<ChunkCoordinate>* evicted_coordinate = nullptr
    );

    [[nodiscard]]
    bool setProtectedChunks(std::span<ChunkCoordinate const> coordinates);

    [[nodiscard]]
    bool isChunkProtected(ChunkCoordinate coordinate) const noexcept;

    [[nodiscard]]
    uint64_t protectedChunkCount() const noexcept;

    [[nodiscard]]
    bool setTerrainIdentity(TerrainIdentity identity);

    // Only a caller with a validated server/session authority receipt may publish this full-volume profile.
    [[nodiscard]] bool publishAuthoritativeSurface(SurfaceTile const& tile);
    [[nodiscard]] bool publishAuthoritativeSurface(AuthoritativeTerrainTile const& tile);
    [[nodiscard]] std::optional<AuthoritativeTerrainTile> authoritativeTile(ChunkCoordinate coordinate) const;
    [[nodiscard]] std::optional<AuthoritativeTerrainColumn> authoritativeColumn(int64_t x, int64_t y) const;
    [[nodiscard]] bool hasAuthoritativeChunk(ChunkCoordinate coordinate) const noexcept;
    [[nodiscard]] bool hasAuthoritativeSurface(ChunkCoordinate coordinate) const noexcept;
    [[nodiscard]] std::optional<AuthoritativeTileIdentity> authoritativeTileIdentity(ChunkCoordinate coordinate) const;
    [[nodiscard]]
    bool matchesAuthoritativeTileIdentity(
        ChunkCoordinate coordinate,
        TerrainIdentity const& identity,
        uint64_t revision,
        uint64_t content_hash
    ) const;
    [[nodiscard]] bool setProtectedAuthoritativeColumns(std::span<ChunkCoordinate const> coordinates);
    [[nodiscard]] bool removeAuthoritativeSurface(ChunkCoordinate coordinate) noexcept;
    [[nodiscard]] std::vector<ChunkCoordinate> availableAuthoritativeColumns() const;
    [[nodiscard]] uint64_t authoritativeSurfaceRevision() const noexcept { return m_authoritative_surface_revision; }

    [[nodiscard]]
    bool publishSurfaceMetadata(SurfaceTile const& tile);

    [[nodiscard]]
    bool removeSurfaceMetadata(ChunkCoordinate coordinate);

    [[nodiscard]]
    std::optional<VerifiedSurfaceColumn> verifiedSurfaceColumn(
        int64_t world_x,
        int64_t world_y
    ) const;

    [[nodiscard]]
    uint64_t surfaceMetadataTileCount() const noexcept;

    void setWorldIdentity(uint64_t revision, uint64_t seed) noexcept;

    [[nodiscard]]
    bool setBlock(WorldCoordinate coordinate, Block block);

    [[nodiscard]]
    Chunk const* residentChunk(ChunkCoordinate coordinate) const noexcept;

    [[nodiscard]]
    Chunk* ensureChunk(
        ChunkCoordinate coordinate,
        std::optional<ChunkCoordinate>* evicted_coordinate = nullptr
    );

    [[nodiscard]]
    bool evict(ChunkCoordinate coordinate) noexcept;

    [[nodiscard]]
    uint64_t residentChunkCount() const noexcept;

    [[nodiscard]]
    uint64_t maxResidentChunks() const noexcept;

    [[nodiscard]]
    uint64_t previewCount() const noexcept;

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
        uint64_t authoritative_baseline_hash = 0U;
    };

    struct ResidentEvictionCandidate final {
        uint64_t last_access;
        ChunkCoordinate coordinate;
    };

    struct ResidentEvictionCandidateLess final {
        [[nodiscard]]
        bool operator()(
            ResidentEvictionCandidate const& left,
            ResidentEvictionCandidate const& right
        ) const noexcept;
    };

    using ResidentChunks = std::unordered_map<ChunkCoordinate, ResidentChunk, ChunkCoordinateHash>;
    using ProtectedChunks = std::unordered_set<ChunkCoordinate, ChunkCoordinateHash>;
    using ResidentEvictionCandidates = std::set<
        ResidentEvictionCandidate,
        ResidentEvictionCandidateLess
    >;

    using SurfaceMaterialProfile = AuthoritativeMaterialProfile;

    struct AuthoritativeSurface final {
        TerrainIdentity identity;
        uint32_t block_sea_level = 0U;
        uint32_t max_height = WorldExtent::DEPTH - 1U;
        std::array<SurfaceMaterialProfile, Chunk::FACE_BLOCK_COUNT> columns{};
        uint64_t revision = 0U;
        uint64_t dense_mask = 0U;
        bool edited = false;
    };

    struct SurfaceMetadata final {
        TerrainIdentity identity;
        uint32_t block_sea_level = 0U;
        std::array<SurfaceMaterialProfile, Chunk::FACE_BLOCK_COUNT> columns{};
        std::array<ChunkContentIdentity, WorldExtent::DEPTH / Chunk::SIDE_LENGTH> chunk_identities{};
        std::bitset<WorldExtent::DEPTH / Chunk::SIDE_LENGTH> resident_chunks;
        uint64_t last_access = 0U;
        bool proof_ready = false;
    };

    using SurfaceMetadataTiles = std::unordered_map<ChunkCoordinate, SurfaceMetadata, ChunkCoordinateHash>;

    struct Preview final {
        HeightTile tile;
        uint64_t revision;
        uint64_t seed;
    };

    using PreviewChunks = std::unordered_map<ChunkCoordinate, Preview, ChunkCoordinateHash>;

    [[nodiscard]]
    uint64_t authoritativeDenseSectionMask(ChunkCoordinate coordinate, AuthoritativeSurface const& tile) const;
    [[nodiscard]]
    uint64_t authoritativeTileContentHash(ChunkCoordinate coordinate, AuthoritativeSurface const& tile) const;
    void pinAuthoritativeSurface(ChunkCoordinate coordinate) noexcept;
    void advanceAccessClock() noexcept;
    void touch(ChunkCoordinate coordinate, ResidentChunk& resident) noexcept;
    void indexResidentForEviction(ChunkCoordinate coordinate, ResidentChunk const& resident);
    void touchSurfaceMetadata(SurfaceMetadata& metadata) const noexcept;
    void invalidateSurfaceMetadata(ChunkCoordinate coordinate) const noexcept;
    [[nodiscard]]
    bool matchesSurfaceMetadata(
        SurfaceMetadata const& metadata,
        Chunk const& chunk,
        ChunkCoordinate coordinate
    ) const noexcept;
    [[nodiscard]]
    bool evictIfFull(std::optional<ChunkCoordinate>* evicted_coordinate);

private:
    SparseWorldOptions m_options;
    std::optional<TerrainGenerator> m_generator;
    std::shared_ptr<TerrainWorld const> m_terrain_world;
    std::optional<TerrainIdentity> m_terrain_identity;
    ResidentChunks m_resident;
    PreviewChunks m_previews;
    ProtectedChunks m_protected_chunks;
    ResidentEvictionCandidates m_evictable_residents;
    mutable SurfaceMetadataTiles m_surface_metadata;
    std::unordered_map<ChunkCoordinate, AuthoritativeSurface, ChunkCoordinateHash> m_authoritative_surfaces;
    std::map<uint64_t, ChunkCoordinate> m_evictable_authoritative_surfaces;
    ProtectedChunks m_protected_authoritative_columns;
    ProtectedChunks m_authoritative_dense_overrides;
    uint64_t m_authoritative_surface_revision = 0U;
    uint64_t m_access_clock = 0U;
    mutable uint64_t m_surface_metadata_clock = 0U;
};

} // namespace shared
