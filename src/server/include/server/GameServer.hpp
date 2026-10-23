#pragma once

#include <server/HeightTileWorkerPool.hpp>
#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>
#include <core/net/Event.hpp>
#include <core/net/Server.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace server {

class WorldPreparation;

class GameServer final : public core::Server {
public:
    using WorkerMetrics = HeightTileWorkerMetrics;

    enum class BindMode : uint8_t {
        AnyHost,
        Loopback,
    };

    struct PreviewStreamMetrics final {
        struct PhaseMetrics final {
            uint64_t calls = 0U;
            std::chrono::nanoseconds duration{};
        };

        WorkerMetrics height_tile_jobs;
        WorkerMetrics world_generation_jobs;
        // Phase totals are accumulated between on_preview_metrics callbacks.
        PhaseMetrics coverage_refresh;
        PhaseMetrics worker_result_publication;
        PhaseMetrics queue_refill;
        PhaseMetrics delivery;
        PhaseMetrics worker_dispatch;
        uint64_t materialization_candidate_keys_examined = 0U;
        uint64_t materialization_resident_chunk_lookups = 0U;
        uint64_t materialization_admission_attempts = 0U;
        uint64_t materialization_admissions = 0U;
        // Forecast keys visited by the reporting player's queue refill since its previous callback.
        uint32_t forecast_materialization_candidates_examined = 0U;
        uint64_t materialization_duplicate_requests = 0U;
        uint64_t chunk_delivery_candidate_keys_examined = 0U;
        uint64_t chunk_delivery_resident_chunk_lookups = 0U;
        uint64_t chunk_delivery_batches_sent = 0U;
        uint64_t chunk_delivery_chunks_sent = 0U;
        uint64_t chunk_delivery_credits_consumed = 0U;
        uint64_t world_generation_jobs_dispatched = 0U;
        uint64_t height_tile_jobs_dispatched = 0U;
        // This count covers the server's entire SparseWorld, not just the active sphere.
        uint64_t server_world_resident_chunks = 0U;
        uint32_t required_near_chunk_keys = 0U;
        uint32_t required_full_chunk_keys = 0U;
        uint32_t acknowledged_chunk_keys = 0U;
        uint32_t inflight_chunk_batches = 0U;
        uint32_t inflight_chunk_keys = 0U;
        uint32_t chunk_delivery_credits = 0U;
        bool near_required_chunks_all_resident = false;
        bool terrain_center_valid = false;
        int32_t terrain_center_x = 0;
        int32_t terrain_center_y = 0;
        uint32_t near_window_desired_tiles = 0U;
        uint32_t near_window_resident_tiles = 0U;
        uint32_t near_window_queued_tiles = 0U;
        uint32_t near_window_dispatched_tiles = 0U;
        uint32_t near_window_ready_tiles = 0U;
        uint32_t near_window_inflight_additions = 0U;
        uint32_t near_window_inflight_removals = 0U;
        uint32_t near_window_untracked_tiles = 0U;
        uint32_t queued_tiles = 0U;
        uint32_t priority_scan_cursor = 0U;
        uint32_t background_scan_cursor = 0U;
        bool background_generation_eligible = false;
        uint32_t dispatched_tiles = 0U;
        uint32_t pending_worker_jobs = 0U;
        uint32_t submitted_worker_jobs = 0U;
        uint32_t ready_tiles = 0U;
        uint32_t inflight_deliveries = 0U;
        uint32_t inflight_additions = 0U;
        uint32_t inflight_removals = 0U;
        uint32_t pending_removals = 0U;
        uint32_t delivery_credits = 0U;
        uint32_t resident_tiles = 0U;
        uint32_t admitted_additions = 0U;
        uint64_t rejected_delivery_acknowledgements = 0U;
        uint32_t latest_received_input_sequence = 0U;
        uint32_t acknowledged_input_sequence = 0U;
        uint32_t unacknowledged_input_count = 0U;
        uint32_t pending_input_count = 0U;
        uint32_t authoritative_generation_cursor = 0U;
        uint32_t authoritative_delivery_cursor = 0U;
        uint32_t authoritative_pending_jobs = 0U;
        uint32_t authoritative_inflight_batches = 0U;
        uint32_t acknowledged_authoritative_columns = 0U;
        uint64_t regional_artifact_bytes = 0U;
        uint32_t materialization_admission_failures = 0U;
        uint64_t delivery_credit_samples = 0U;
        std::chrono::nanoseconds delivery_credit_total{};
        std::chrono::nanoseconds delivery_credit_max{};
        std::chrono::nanoseconds server_loop_interval{};
        std::chrono::nanoseconds server_loop_work{};
        std::chrono::nanoseconds server_tick{};
        std::chrono::nanoseconds stream_pump{};
        std::chrono::nanoseconds network_poll{};
    };
    struct BenchmarkHooks final {
        std::function<void(std::chrono::nanoseconds, uint64_t)> on_tick;
        std::function<void(core::ClientId, uint32_t)> on_preview_buffered;
        std::function<void(core::ClientId, PreviewStreamMetrics)> on_preview_metrics;
        std::function<void(core::ClientId, shared::HeightTileKey)> before_height_tile_completion;
    };
    struct SpawnPoint final {
        char character;
        int32_t x;
        int32_t y;
        int32_t z = 0;
    };

    explicit GameServer(
        uint16_t port = 20'040,
        std::vector<SpawnPoint> spawn_points = { },
        shared::WorldMode world_mode = shared::WorldMode::Flat,
        shared::WorldConfiguration configuration = shared::World::canonicalConfiguration(),
        uint32_t render_distance = shared::HEIGHT_TILE_INTEREST_RADIUS,
        std::shared_ptr<shared::TerrainWorld const> terrain_world = nullptr,
        std::shared_ptr<WorldPreparation> world_preparation = nullptr,
        BindMode bind_mode = BindMode::AnyHost
    );

    ~GameServer() override;

    [[nodiscard]]
    static std::expected<std::vector<SpawnPoint>, std::string> validateSpawnPoints(
        std::vector<SpawnPoint> spawn_points,
        shared::WorldMode world_mode = shared::WorldMode::Flat
    );

    [[nodiscard]]
    static std::chrono::milliseconds fixedTickDelay(std::chrono::milliseconds elapsed) noexcept;

    [[nodiscard]]
    static uint32_t terrainWorkerCount(uint32_t hardware_concurrency) noexcept;

    void run();
    void run(std::atomic_bool const& stop_requested);
    void run(std::atomic_bool const& stop_requested, BenchmarkHooks const* benchmark_hooks);
    [[nodiscard]]
    uint64_t tick(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

    [[nodiscard]] bool rerootPlayerCoverage(shared::PlayerId player_id, shared::PlayerPosition position);

    [[nodiscard]]
    std::expected<uint64_t, shared::PolicyDiagnostic> publishPermissions(
        shared::PolicyCompilation compilation,
        shared::PolicyCapabilityRegistry registry
    );
private:
    static constexpr uint32_t MAX_TERRAIN_INFLIGHT_BYTES = 49'152U;
    static constexpr uint32_t MAX_PREPARED_STREAM_INFLIGHT_BYTES = 16'384U;

    struct PreviewStream;
    void populateNearWindowMetrics(PreviewStream const& stream, PreviewStreamMetrics& metrics) const;

    struct PlayerReplication final {
        static constexpr uint32_t MAX_PENDING_INPUTS = 64;
        static constexpr uint32_t MAX_MATERIALIZATION_ADMISSION_FAILURES = 8;

        struct ChunkCoordinateHash final {
            [[nodiscard]] size_t operator()(shared::ChunkCoordinate coordinate) const noexcept;
        };

        using ChunkKeySet = std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash>;

        struct TerrainDelivery final {
            std::vector<shared::ChunkCoordinate> coordinates;
            uint32_t encoded_bytes = 0U;
            uint64_t coverage_epoch = 0U;
        };

        shared::PlayerId id;
        uint32_t latest_received_sequence = 0;
        uint32_t acknowledged_input_sequence = 0;
        uint32_t state_revision = 1;
        std::deque<shared::ClientInputMessage> pending_inputs;
        bool has_received_sequence = false;
        bool action_consumed_this_tick = false;
        uint32_t materialization_admission_failures = 0U;
        uint32_t forecast_materialization_candidates_since_metrics = 0U;
        std::optional<shared::Direction> last_accepted_direction;
        std::optional<shared::TerrainSessionIdentity> terrain_session;
        uint64_t coverage_epoch = 0U;
        bool wants_previews = false;
        bool has_chunk_coverage = false;
        shared::PlayerPosition coverage_center{};
        bool chunk_credit_grant_received = false;
        uint8_t chunk_delivery_credits = 0U;
        bool benchmark_near_chunks_all_resident = false;
        bool world_started = false;
        uint64_t next_chunk_delivery_token = 1U;
        size_t next_chunk_materialization_index = 0U;
        std::vector<shared::ChunkCoordinate>::size_type next_prefetch_materialization_index = 0U;
        std::optional<shared::Direction> ordered_chunk_direction;
        std::vector<shared::ChunkCoordinate> ordered_chunk_keys;
        std::vector<shared::ChunkCoordinate> ordered_prefetch_chunk_keys;
        ChunkKeySet required_near_chunks;
        ChunkKeySet required_chunks;
        ChunkKeySet required_interest_chunks;
        ChunkKeySet forecast_lead_chunks;
        std::vector<shared::ChunkCoordinate> ordered_authoritative_columns;
        ChunkKeySet required_authoritative_columns;
        ChunkKeySet authoritative_interest_columns;
        ChunkKeySet acknowledged_authoritative_columns;
        ChunkKeySet inflight_authoritative_columns;
        ChunkKeySet scheduled_authoritative_columns;
        std::unordered_map<uint64_t, shared::ChunkCoordinate> authoritative_jobs;
        std::unordered_map<uint64_t, TerrainDelivery> authoritative_deliveries;
        uint64_t next_authoritative_delivery_token = 1U;
        uint32_t next_authoritative_generation_index = 0U;
        uint32_t next_authoritative_delivery_index = 0U;
        std::deque<uint32_t> authoritative_delivery_frontier;
        uint64_t authoritative_publication_failures = 0U;
        ChunkKeySet acknowledged_chunks;
        // Ready keys occupy one priority queue; waiting keys are nonresident interest candidates.
        ChunkKeySet ready_delivery_frontier_keys;
        std::deque<shared::ChunkCoordinate> current_delivery_frontier;
        ChunkKeySet current_delivery_queue_keys;
        std::deque<shared::ChunkCoordinate> near_delivery_frontier;
        ChunkKeySet near_delivery_queue_keys;
        std::deque<shared::ChunkCoordinate> prefetch_delivery_frontier;
        ChunkKeySet prefetch_delivery_queue_keys;
        ChunkKeySet waiting_delivery_frontier;
        ChunkKeySet inflight_delivery_keys;
        ChunkKeySet scheduled_chunks;
        std::unordered_map<uint64_t, TerrainDelivery> inflight_chunk_deliveries;

        [[nodiscard]]
        bool hasAuthoritativeTerrainWork() const noexcept
        {
            return terrain_session && has_chunk_coverage
                && (next_authoritative_generation_index < ordered_authoritative_columns.size()
                    || !authoritative_delivery_frontier.empty()
                    || !authoritative_jobs.empty() || !authoritative_deliveries.empty());
        }
    };

    struct PendingTerrainJoin final {
        core::ClientId client_id = 0U;
        char character = '@';
        shared::TerrainSessionIdentity session;
        bool wants_previews = false;
        uint64_t last_progress_source_sequence = 0U;
        uint64_t next_message_sequence = 1U;
        uint32_t next_default_spawn_candidate = 0U;
        std::optional<shared::HeightTileKey> requested_surface_key;
        std::optional<shared::HeightTileKey> surface_key;
        std::optional<shared::SurfaceTile> surface_tile;
        std::vector<shared::ChunkCoordinate> generation_requests;
    };

    void onConnected(core::ServerConnectEvent const event) override;
    void onDisconnected(core::ServerDisconnectEvent const event) override;
    void onReceived(core::ServerReceiveEvent event) override;

    void send(shared::Message message);
    void sendTo(std::optional<core::ClientId> const client_id, shared::Message message);
    [[nodiscard]]
    uint32_t remainingTerrainDeliveryBytes(core::ClientId client_id) const noexcept;
    void sendHeightTileTo(std::optional<core::ClientId> const client_id, shared::Message message);
    void startHeightTileStream(core::ClientId client_id);
    [[nodiscard]]
    uint64_t pollNetworkBatch(
        std::chrono::milliseconds timeout,
        std::atomic_bool const* stop_requested
    );
    [[nodiscard]]
    uint64_t tickImpl(
        std::chrono::milliseconds timeout,
        std::atomic_bool const* stop_requested
    );
    void acknowledgeHeightTileDelivery(
        core::ClientId client_id,
        shared::ClientHeightTileCreditMessage const& credit
    );
    void refreshHeightTileInterest(PreviewStream& stream, shared::Player const& player);
    void fillHeightTileQueue(PreviewStream& stream);
    void processAuthoritativeTerrain();
    bool publishAuthoritativeTerrainResult(HeightTileWorkerPool::Result const& result);
    void acknowledgeAuthoritativeTerrain(
        core::ClientId client_id, shared::ClientAuthoritativeTerrainAckMessage const& acknowledgement
    );
    void processHeightTileStreams();
    void dispatchHeightTileWork();
    void publishHeightTileResults();
    void dispatchWorldMaterialization();
    void processWorldPreparation();
    void initializePreparedWorld(std::shared_ptr<shared::TerrainWorld const> terrain_world);
    void activatePreparedJoin(PendingTerrainJoin join, shared::PlayerPosition spawn);
    [[nodiscard]] std::expected<std::optional<shared::PlayerPosition>, std::string> safeSpawnFor(
        PendingTerrainJoin& join
    );
    [[nodiscard]] shared::GenerationAdmission requestPendingJoinPlan(
        PendingTerrainJoin& join,
        shared::ChunkCoordinate coordinate,
        shared::WorldGenerationPlan plan
    );
    void cancelPendingTerrainJoinWork(PendingTerrainJoin const& join) noexcept;
    void publishWorldPreparation(
        PendingTerrainJoin& join,
        generation::GenerationProgress const* progress,
        std::shared_ptr<generation::GenerationSnapshot const> snapshot,
        bool final_atlas_complete,
        std::optional<shared::PlayerPosition> safe_spawn = std::nullopt,
        std::string failure = {}
    );
    void refreshMaterializedCoverage();
    void fillMaterializedChunkQueue();
    void pumpMaterializedChunkDeliveries();
    void acknowledgeMaterializedChunkDelivery(
        core::ClientId client_id,
        shared::ClientChunkDeliveryAckMessage const& acknowledgement
    );
    void enqueueMaterializedDeliveryCandidate(
        PlayerReplication& replication,
        shared::ChunkCoordinate coordinate
    );
    void enqueueMaterializedDeliveryNeighbors(
        PlayerReplication& replication,
        shared::ChunkCoordinate coordinate
    );
    void refreshMaterializedDeliveryFrontier(PlayerReplication& replication, bool seed_connected);
    [[nodiscard]]
    PlayerReplication::ChunkKeySet connectedMaterializedDeliveryKeys(
        PlayerReplication const& replication
    ) const;
    void startPreparedWorld(
        core::ClientId client_id,
        shared::ClientStartWorldMessage const& start
    );
    void resetChunkCoverage(PlayerReplication& replication, shared::PlayerPosition center, bool force_new_epoch);
    void rebuildMaterializedChunkOrders(
        PlayerReplication& replication,
        shared::PlayerPosition center,
        std::optional<shared::Direction> direction
    );
    [[nodiscard]]
    std::optional<shared::Direction> materializedInterestDirection(PlayerReplication const& replication) const noexcept;
    [[nodiscard]] bool nearChunksAreReady(PlayerReplication const& replication) const noexcept;
    void updateProtectedChunkUnion();
    [[nodiscard]] uint32_t admitHeightTileDeliveries(PreviewStream& stream, uint32_t maximum_batches);
    [[nodiscard]] bool processInput(
        PlayerReplication& replication,
        shared::ClientInputMessage input,
        bool acknowledge_input = true
    );
    [[nodiscard]] std::vector<shared::PolicySubject> permissionSubjects() const;
    [[nodiscard]]
    bool canApplyPublishedPermissions(
        shared::PolicyCapabilitySnapshot const& capabilities,
        shared::PolicyCapabilityRegistry const& registry
    ) const;
    [[nodiscard]]
    std::expected<uint64_t, shared::PolicyDiagnostic> refreshPublishedPermissions(
        std::optional<shared::PolicyEntityId> expired_subject = std::nullopt
    );
    void applyPublishedPermissions();
    [[nodiscard]]
    PlayerReplication* playerReplication(shared::PlayerId id) noexcept;
    [[nodiscard]]
    shared::ServerPlayerPositionMessage playerPositionMessage(
        shared::Player const& player,
        PlayerReplication const& replication
    ) const noexcept;
    [[nodiscard]]
    static std::vector<SpawnPoint> checkedSpawnPoints(
        std::vector<SpawnPoint> spawn_points,
        shared::WorldMode world_mode
    );
private:
    struct StreamingMetricsAccumulator final {
        PreviewStreamMetrics::PhaseMetrics coverage_refresh;
        PreviewStreamMetrics::PhaseMetrics worker_result_publication;
        PreviewStreamMetrics::PhaseMetrics queue_refill;
        PreviewStreamMetrics::PhaseMetrics delivery;
        PreviewStreamMetrics::PhaseMetrics worker_dispatch;
        uint64_t materialization_candidate_keys_examined = 0U;
        uint64_t materialization_resident_chunk_lookups = 0U;
        uint64_t materialization_admission_attempts = 0U;
        uint64_t materialization_admissions = 0U;
        uint64_t materialization_duplicate_requests = 0U;
        uint64_t chunk_delivery_candidate_keys_examined = 0U;
        uint64_t chunk_delivery_resident_chunk_lookups = 0U;
        uint64_t chunk_delivery_batches_sent = 0U;
        uint64_t chunk_delivery_chunks_sent = 0U;
        uint64_t chunk_delivery_credits_consumed = 0U;
        uint64_t world_generation_jobs_dispatched = 0U;
        uint64_t height_tile_jobs_dispatched = 0U;
    };

    shared::World m_world;
    std::shared_ptr<shared::TerrainWorld const> m_terrain_world;
    std::shared_ptr<WorldPreparation> m_world_preparation;
    std::unique_ptr<shared::SparseWorld> m_physics_world;
    bool m_prepared_world_initialized = false;
    shared::WorldGenerationCoordinator m_world_generation;
    std::shared_ptr<shared::TerrainGenerator const> m_terrain_generator;
    shared::WorldGenerationPlan m_generation_plan;
    shared::PolicyHost m_permission_host;
    shared::PolicyCapabilityRegistry m_permission_registry;
    std::optional<shared::PolicyCapabilityKeyId> m_flight_permission;
    std::optional<shared::PolicyCapabilityKeyId> m_collision_bypass_permission;
    bool m_permissions_published{false};
    std::vector<SpawnPoint> m_spawn_points;
    std::vector<PlayerReplication> m_player_replications;
    std::vector<PendingTerrainJoin> m_pending_terrain_joins;
    struct PreviewStream final {
        struct HeightTileKeyHash final {
            [[nodiscard]] size_t operator()(shared::HeightTileKey const key) const noexcept
            {
                return static_cast<size_t>(shared::heightTileCoordinateHash(key.x, key.y));
            }
        };

        static constexpr uint32_t MAX_QUEUED_TILES = 128U;
        static constexpr uint32_t MAX_BUFFERED_TILES = 128U;
        static constexpr uint32_t MAX_INFLIGHT_DELIVERIES = shared::HEIGHT_TILE_DELIVERY_WINDOW;
        // 13-byte batch framing plus the largest surface and 16-bit height-delta payload.
        static constexpr uint32_t MAX_DELIVERY_BYTES = 10'118U;
        static constexpr uint32_t MAX_INFLIGHT_DELIVERY_BYTES = 34'356U;
        static constexpr uint32_t MAX_URGENT_CANDIDATES_PER_PUMP = 512U;
        static constexpr uint32_t MAX_PRIORITY_CANDIDATES_PER_PUMP = 256U;
        static constexpr uint32_t MAX_BACKGROUND_CANDIDATES_PER_PUMP = 512U;
        static constexpr uint64_t WORLD_REVISION = 1U;

        struct Delivery final {
            std::vector<shared::HeightTileKey> additions;
            std::vector<shared::HeightTileKey> removals;
            uint32_t encoded_bytes = 0U;
            std::chrono::steady_clock::time_point admitted_at{};
        };

        struct RemovalCandidate final {
            shared::HeightTileKey key;
            double priority;
        };

        explicit PreviewStream(core::ClientId client_id, uint32_t const render_distance)
            : client_id(client_id)
            , scheduler(MAX_QUEUED_TILES)
            , render_distance(render_distance)
        {}

        [[nodiscard]] bool desires(shared::HeightTileKey const key) const
        {
            return has_center && shared::heightTileWithinInterest(center, key, render_distance);
        }

        core::ClientId client_id;
        shared::WorldGenerationScheduler scheduler;
        uint32_t render_distance;
        shared::HeightTileKey center{};
        uint64_t generation = 1U;
        int8_t heading_x = 0;
        int8_t heading_y = 127;
        int8_t applied_heading_x = 0;
        int8_t applied_heading_y = 0;
        std::optional<shared::Direction> last_actual_direction;
        std::optional<shared::HeightTileKey> forecast_center;
        bool has_center = false;
        std::span<shared::HeightTileKey const> priority_offsets;
        uint32_t priority_cursor = 0U;
        uint64_t priority_cursor_generation = 0U;
        uint32_t background_cursor = 0U;
        uint64_t background_cursor_generation = 0U;
        std::vector<shared::HeightTileKey> urgent_keys;
        uint32_t urgent_cursor = 0U;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> resident_keys;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> queued_keys;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> dispatched_keys;
        std::unordered_map<shared::HeightTileKey, shared::ServerHeightTileMessage, HeightTileKeyHash> ready_tiles;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> pending_removals;
        std::vector<RemovalCandidate> removal_candidates;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> inflight_addition_keys;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> inflight_removal_keys;
        std::unordered_map<uint64_t, Delivery> inflight_deliveries;
        uint8_t delivery_credits = 0U;
        uint64_t delivery_credit_samples = 0U;
        std::chrono::nanoseconds delivery_credit_total{};
        std::chrono::nanoseconds delivery_credit_max{};
        uint32_t admitted_additions_since_metrics = 0U;
        uint64_t rejected_delivery_acknowledgements = 0U;
        double background_generation_tokens = 0.0;
        std::chrono::steady_clock::time_point background_budget_updated_at =
            std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point background_generation_eligible_at =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
    };
    std::vector<PreviewStream> m_preview_streams;
    std::vector<PlayerReplication>::size_type m_next_materialization_admission = 0U;
    size_t m_next_preview_admission = 0U;
    size_t m_next_preview_dispatch = 0U;
    std::shared_ptr<shared::HeightTileInterestOrders const> m_height_tile_interest_orders;
    uint64_t m_next_authoritative_job_id = uint64_t{1U} << 63U;
    std::unique_ptr<HeightTileWorkerPool> m_height_tile_workers;
    uint64_t m_next_height_tile_token = 1;
    bool m_benchmark_metrics_enabled = false;
    std::chrono::nanoseconds m_last_stream_pump_duration{};
    StreamingMetricsAccumulator m_streaming_metrics;
};

} // namespace server
