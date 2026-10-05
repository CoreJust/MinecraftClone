#pragma once

#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <core/net/Server.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace server {

class GameServer final : public core::Server {
public:
    struct WorkerMetrics final {
        uint32_t pending_jobs = 0U;
        uint32_t executor_queued_jobs = 0U;
        uint32_t running_jobs = 0U;
        uint32_t completed_uncollected_jobs = 0U;
        uint64_t enqueued_total = 0U;
        uint64_t submitted_total = 0U;
        uint64_t started_total = 0U;
        uint64_t finished_total = 0U;
        uint64_t collected_total = 0U;
        std::chrono::nanoseconds enqueue_to_submit_total{};
        std::chrono::nanoseconds enqueue_to_submit_max{};
        std::chrono::nanoseconds enqueue_to_start_total{};
        std::chrono::nanoseconds enqueue_to_start_max{};
        std::chrono::nanoseconds executor_queue_total{};
        std::chrono::nanoseconds executor_queue_max{};
        std::chrono::nanoseconds execution_total{};
        std::chrono::nanoseconds execution_max{};
        std::chrono::nanoseconds completion_to_collection_total{};
        std::chrono::nanoseconds completion_to_collection_max{};
    };

    struct PreviewStreamMetrics final {
        WorkerMetrics height_tile_jobs;
        WorkerMetrics world_generation_jobs;
        uint32_t queued_tiles = 0U;
        uint32_t dispatched_tiles = 0U;
        uint32_t pending_worker_jobs = 0U;
        uint32_t submitted_worker_jobs = 0U;
        uint32_t ready_tiles = 0U;
        uint32_t inflight_deliveries = 0U;
        uint32_t inflight_additions = 0U;
        uint32_t delivery_credits = 0U;
        uint32_t resident_tiles = 0U;
        uint32_t latest_received_input_sequence = 0U;
        uint32_t acknowledged_input_sequence = 0U;
        uint32_t unacknowledged_input_count = 0U;
        uint32_t pending_input_count = 0U;
        uint32_t materialization_admission_failures = 0U;
        uint64_t delivery_credit_samples = 0U;
        std::chrono::nanoseconds delivery_credit_total{};
        std::chrono::nanoseconds delivery_credit_max{};
        std::chrono::nanoseconds server_loop_interval{};
        std::chrono::nanoseconds server_loop_work{};
        std::chrono::nanoseconds server_tick{};
        std::chrono::nanoseconds stream_pump{};
        std::chrono::nanoseconds previous_sleep{};
    };
    struct BenchmarkHooks final {
        std::function<void(std::chrono::nanoseconds, uint64_t)> on_tick;
        std::function<void(core::ClientId, uint32_t)> on_preview_buffered;
        std::function<void(core::ClientId, PreviewStreamMetrics)> on_preview_metrics;
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
        uint32_t render_distance = shared::HEIGHT_TILE_INTEREST_RADIUS
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

    [[nodiscard]]
    std::expected<uint64_t, shared::PolicyDiagnostic> publishPermissions(
        shared::PolicyCompilation compilation,
        shared::PolicyCapabilityRegistry registry
    );
private:
    struct HeightTileWorkerPool;
    struct PreviewStream;

    struct PlayerReplication final {
        static constexpr uint32_t MAX_PENDING_INPUTS = 64;
        static constexpr uint32_t MAX_MATERIALIZATION_ADMISSION_FAILURES = 8;

        shared::PlayerId id;
        uint32_t latest_received_sequence = 0;
        uint32_t acknowledged_input_sequence = 0;
        uint32_t state_revision = 1;
        std::deque<shared::ClientInputMessage> pending_inputs;
        bool has_received_sequence = false;
        bool action_consumed_this_tick = false;
        uint32_t materialization_admission_failures = 0U;
    };

    void onConnected(core::ServerConnectEvent const event) override;
    void onDisconnected(core::ServerDisconnectEvent const event) override;
    void onReceived(core::ServerReceiveEvent event) override;

    void send(shared::Message const message);
    void sendTo(std::optional<core::ClientId> const client_id, shared::Message message);
    void sendHeightTileTo(std::optional<core::ClientId> const client_id, shared::Message message);
    void startHeightTileStream(core::ClientId client_id);
    void acknowledgeHeightTileDelivery(
        core::ClientId client_id,
        shared::ClientHeightTileCreditMessage const& credit
    );
    void refreshHeightTileInterest(PreviewStream& stream, shared::Player const& player);
    void fillHeightTileQueue(PreviewStream& stream);
    void processHeightTileStreams();
    void dispatchHeightTileWork();
    void publishHeightTileResults();
    void dispatchWorldMaterialization();
    [[nodiscard]] uint32_t admitHeightTileDeliveries(PreviewStream& stream, uint32_t maximum_batches);
    void queueDepartedResidentTiles(PreviewStream& stream);
    [[nodiscard]] bool processInput(PlayerReplication& replication, shared::ClientInputMessage input);
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
    shared::World m_world;
    shared::SparseWorld m_physics_world;
    shared::WorldGenerationCoordinator m_world_generation;
    std::shared_ptr<shared::TerrainGenerator const> m_terrain_generator;
    shared::WorldGenerationPlan m_generation_plan{
        .chunk_stages = {shared::GenerationStage::Materialize},
    };
    shared::PolicyHost m_permission_host;
    shared::PolicyCapabilityRegistry m_permission_registry;
    std::optional<shared::PolicyCapabilityKeyId> m_flight_permission;
    std::optional<shared::PolicyCapabilityKeyId> m_collision_bypass_permission;
    bool m_permissions_published{false};
    std::vector<SpawnPoint> m_spawn_points;
    std::vector<PlayerReplication> m_player_replications;
    struct PreviewStream final {
        struct HeightTileKeyHash final {
            [[nodiscard]] size_t operator()(shared::HeightTileKey const key) const noexcept
            {
                return static_cast<size_t>(shared::heightTileCoordinateHash(key.x, key.y));
            }
        };

        static constexpr uint32_t MAX_QUEUED_TILES = 128U;
        static constexpr uint32_t MAX_BUFFERED_TILES = 128U;
        static constexpr uint32_t MAX_INFLIGHT_DELIVERIES = 4U;
        static constexpr uint64_t WORLD_REVISION = 1U;

        struct Delivery final {
            std::vector<shared::HeightTileKey> additions;
            std::vector<shared::HeightTileKey> removals;
            std::chrono::steady_clock::time_point admitted_at{};
        };

        explicit PreviewStream(core::ClientId client_id)
            : client_id(client_id)
            , scheduler(MAX_QUEUED_TILES)
        {}

        core::ClientId client_id;
        shared::WorldGenerationScheduler scheduler;
        shared::HeightTileKey center{};
        uint64_t generation = 1U;
        int8_t heading_x = 0;
        int8_t heading_y = 127;
        int8_t applied_heading_x = 0;
        int8_t applied_heading_y = 0;
        bool has_center = false;
        std::vector<shared::HeightTileKey> desired_keys;
        uint32_t priority_cursor = 0U;
        uint64_t priority_cursor_generation = 0U;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> desired_key_set;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> resident_keys;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> queued_keys;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> dispatched_keys;
        std::unordered_map<shared::HeightTileKey, shared::ServerHeightTileMessage, HeightTileKeyHash> ready_tiles;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> pending_removals;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> inflight_addition_keys;
        std::unordered_set<shared::HeightTileKey, HeightTileKeyHash> inflight_removal_keys;
        std::unordered_map<uint64_t, Delivery> inflight_deliveries;
        uint8_t delivery_credits = 0U;
        uint64_t delivery_credit_samples = 0U;
        std::chrono::nanoseconds delivery_credit_total{};
        std::chrono::nanoseconds delivery_credit_max{};
        double background_generation_tokens = 0.0;
        std::chrono::steady_clock::time_point background_budget_updated_at =
            std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point background_generation_eligible_at =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
    };
    std::vector<PreviewStream> m_preview_streams;
    size_t m_next_preview_admission = 0U;
    size_t m_next_preview_dispatch = 0U;
    std::shared_ptr<shared::HeightTileInterestOrders const> m_height_tile_interest_orders;
    std::unique_ptr<HeightTileWorkerPool> m_height_tile_workers;
    uint64_t m_next_height_tile_token = 1;
    bool m_benchmark_metrics_enabled = false;
    std::chrono::nanoseconds m_last_stream_pump_duration{};
};

} // namespace server
