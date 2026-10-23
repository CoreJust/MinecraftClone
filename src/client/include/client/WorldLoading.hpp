#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <generation/Generation.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace client
{

enum class WorldLoadingPlatform : uint8_t
{
    Mac,
    Windows,
};

enum class WorldLoadingState : uint8_t
{
    Idle,
    Creating,
    PreparingAtlas,
    PreparingSpawn,
    ReadyForStart,
    Starting,
    Playing,
    Failed,
    Cancelled,
};

enum class SpawnValidation : uint8_t
{
    Unknown,
    Unsafe,
    Safe,
};

enum class LoadingInputResult : uint8_t
{
    Forward,
    Consumed,
    Started,
};

struct WorldLoadingIdentity final
{
    uint64_t join_epoch = 0U;
    generation::GenerationConfiguration configuration;

    [[nodiscard]]
    bool operator==(WorldLoadingIdentity const&) const = default;
};

struct WorldLoadingStatus final
{
    WorldLoadingState state = WorldLoadingState::Idle;
    std::chrono::steady_clock::time_point request_accepted_at{};
    std::chrono::steady_clock::time_point ready_prompt_deadline{};
    std::chrono::steady_clock::time_point full_radius_deadline{};
    std::optional<std::chrono::steady_clock::time_point> atlas_published_at;
    std::optional<std::chrono::steady_clock::time_point> refined_spawn_at;
    std::optional<std::chrono::steady_clock::time_point> initial_sphere_ready_at;
    std::optional<std::chrono::steady_clock::time_point> ready_prompt_frame_rendered_at;
    std::optional<std::chrono::steady_clock::time_point> key_accepted_at;
    std::optional<std::chrono::steady_clock::time_point> start_acknowledged_at;
    std::optional<std::chrono::steady_clock::time_point> playable_sphere_first_ready_at;
    std::optional<std::chrono::steady_clock::time_point> startup_prefetch_primed_at;
    std::optional<shared::PlayerPosition> safe_spawn_position;
    std::optional<shared::PlayerPosition> current_player_position;
    uint32_t initial_sphere_ready_chunks = 0U;
    uint32_t initial_sphere_required_chunks = 0U;
    // Full-volume chunk equivalents; authoritative column counters below count XY profiles.
    uint32_t playable_sphere_ready_chunks = 0U;
    uint32_t playable_sphere_required_chunks = 0U;
    uint32_t resident_authoritative_columns = 0U;
    uint32_t required_authoritative_columns = 0U;
    uint32_t actual_chunk_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    uint32_t visual_lod_radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS;
    generation::GenerationStage generation_stage = generation::GenerationStage::Tectonics;
    uint32_t generation_completed = 0U;
    uint32_t generation_total = 0U;
    std::optional<uint64_t> server_creation_elapsed_microseconds;
    bool atlas_ready = false;
    bool start_pending = false;
    bool spawn_safe = false;
    bool initial_sphere_ready = false;
    bool playable_sphere_ready = false;
    bool ready_prompt_deadline_missed = false;
    bool full_radius_deadline_missed = false;
    std::shared_ptr<generation::GenerationSnapshot const> map_snapshot;
    std::shared_ptr<generation::GlobalAtlas const> atlas;
    std::string error;
};

class WorldLoading final
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr uint32_t INITIAL_READY_SPHERE_RADIUS_BLOCKS = shared::NEAR_TERRAIN_RADIUS_BLOCKS;
    static constexpr uint32_t PLAYABLE_TERRAIN_RADIUS_BLOCKS = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    static constexpr uint32_t PLAYABLE_SPHERE_RADIUS_BLOCKS = PLAYABLE_TERRAIN_RADIUS_BLOCKS;

    explicit WorldLoading(WorldLoadingPlatform platform) noexcept;

    [[nodiscard]]
    bool begin(WorldLoadingIdentity identity, TimePoint accepted_at);

    [[nodiscard]]
    bool restart(WorldLoadingIdentity identity, TimePoint restarted_at);

    [[nodiscard]]
    bool bindConfiguration(
        WorldLoadingIdentity const& identity,
        generation::GenerationConfiguration configuration,
        TimePoint bound_at
    );

    [[nodiscard]]
    bool publishGenerationProgress(
        WorldLoadingIdentity const& identity,
        generation::GenerationProgress const& progress,
        TimePoint published_at
    );

    [[nodiscard]]
    bool publishServerCreationElapsed(
        WorldLoadingIdentity const& identity,
        uint64_t elapsed_microseconds,
        TimePoint published_at
    );

    [[nodiscard]]
    bool publishAtlas(
        WorldLoadingIdentity const& identity,
        std::shared_ptr<generation::GlobalAtlas const> atlas,
        TimePoint published_at
    );

    [[nodiscard]]
    bool publishAtlasCompletion(
        WorldLoadingIdentity const& identity,
        shared::TerrainIdentity const& terrain_identity,
        TimePoint published_at
    );

    [[nodiscard]]
    bool publishRefinedSpawn(
        WorldLoadingIdentity const& identity,
        shared::PlayerPosition position,
        SpawnValidation validation,
        TimePoint refined_at
    );

    [[nodiscard]]
    bool publishMaterializedChunk(
        WorldLoadingIdentity const& identity,
        shared::Chunk const& chunk,
        TimePoint published_at
    );

    [[nodiscard]]
    bool publishAuthoritativeColumn(
        WorldLoadingIdentity const& identity,
        shared::ChunkCoordinate coordinate,
        TimePoint published_at
    );

    [[nodiscard]]
    bool evictAuthoritativeColumn(
        WorldLoadingIdentity const& identity,
        shared::ChunkCoordinate coordinate,
        TimePoint evicted_at
    );

    [[nodiscard]]
    bool evictChunk(
        WorldLoadingIdentity const& identity,
        shared::ChunkCoordinate coordinate,
        TimePoint evicted_at
    );

    [[nodiscard]]
    bool updatePlayerPosition(
        WorldLoadingIdentity const& identity,
        shared::PlayerPosition position,
        TimePoint observed_at
    );

    [[nodiscard]]
    bool markReadyPromptFrameRendered(WorldLoadingIdentity const& identity, TimePoint rendered_at);

    [[nodiscard]]
    LoadingInputResult acceptStartInput(WorldLoadingIdentity const& identity, TimePoint accepted_at);

    [[nodiscard]]
    bool acknowledgeStart(
        WorldLoadingIdentity const& identity,
        bool accepted,
        std::string_view failure,
        TimePoint acknowledged_at
    );

    [[nodiscard]]
    bool fail(WorldLoadingIdentity const& identity, std::string_view reason, TimePoint failed_at);

    [[nodiscard]]
    bool cancel(WorldLoadingIdentity const& identity, TimePoint cancelled_at);

    [[nodiscard]]
    WorldLoadingStatus status(TimePoint now) const;

    [[nodiscard]]
    static std::vector<shared::ChunkCoordinate> requiredChunkKeys(
        shared::PlayerPosition center,
        uint32_t radius_blocks
    );

private:
    struct ChunkCoordinateHash final
    {
        [[nodiscard]]
        uint64_t operator()(shared::ChunkCoordinate coordinate) const noexcept;
    };

    using ChunkKeySet = std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash>;

    [[nodiscard]]
    bool acceptsIdentityAndTime(WorldLoadingIdentity const& identity, TimePoint at) const noexcept;

    [[nodiscard]]
    bool initialSphereReady() const noexcept;

    [[nodiscard]]
    bool playableSphereReady() const noexcept;

    void rebuildInitialSphere(shared::PlayerPosition center);

    void rebuildPlayableSphere(shared::PlayerPosition center);

    void updateReadiness(TimePoint at);

    void refreshReadyState();

    void clearReadiness() noexcept;

    void resetForIdentity(
        WorldLoadingIdentity identity,
        TimePoint event_at,
        TimePoint request_accepted_at
    );

private:
    WorldLoadingPlatform m_platform;
    mutable std::mutex m_mutex;
    WorldLoadingIdentity m_identity;
    bool m_has_identity = false;
    TimePoint m_request_accepted_at{};
    std::optional<TimePoint> m_last_event_at;
    std::optional<TimePoint> m_atlas_published_at;
    std::optional<TimePoint> m_refined_spawn_at;
    std::optional<TimePoint> m_initial_sphere_ready_at;
    std::optional<TimePoint> m_ready_prompt_frame_rendered_at;
    std::optional<TimePoint> m_key_accepted_at;
    std::optional<TimePoint> m_start_acknowledged_at;
    std::optional<TimePoint> m_playable_sphere_first_ready_at;
    std::optional<TimePoint> m_startup_prefetch_primed_at;
    std::optional<shared::PlayerPosition> m_spawn_position;
    std::optional<shared::PlayerPosition> m_player_position;
    ChunkKeySet m_materialized_chunks;
    ChunkKeySet m_initial_required_chunks;
    ChunkKeySet m_authoritative_columns;
    ChunkKeySet m_required_authoritative_columns;
    ChunkKeySet m_startup_required_authoritative_columns;
    uint32_t m_initial_ready_chunks = 0U;
    uint32_t m_ready_authoritative_columns = 0U;
    uint32_t m_startup_ready_authoritative_columns = 0U;
    generation::GenerationStage m_generation_stage = generation::GenerationStage::Tectonics;
    uint32_t m_generation_completed = 0U;
    uint32_t m_generation_total = 0U;
    std::optional<uint64_t> m_server_creation_elapsed_microseconds;
    bool m_has_generation_progress = false;
    std::shared_ptr<generation::GenerationSnapshot const> m_map_snapshot;
    std::shared_ptr<generation::GlobalAtlas const> m_atlas;
    bool m_atlas_ready = false;
    WorldLoadingState m_state = WorldLoadingState::Idle;
    std::string m_error;
};

} // namespace client
