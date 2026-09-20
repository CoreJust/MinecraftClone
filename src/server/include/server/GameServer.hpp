#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <core/net/Server.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace server {

class GameServer final : public core::Server {
public:
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
        shared::WorldConfiguration configuration = shared::World::canonicalConfiguration()
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
    [[nodiscard]]
    uint64_t tick(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
private:
    struct HeightTileWorkerPool;
    struct PreviewStream;

    struct PlayerReplication final {
        static constexpr uint32_t MAX_PENDING_INPUTS = 64;

        shared::PlayerId id;
        uint32_t latest_received_sequence = 0;
        uint32_t acknowledged_input_sequence = 0;
        uint32_t state_revision = 1;
        std::deque<shared::ClientInputMessage> pending_inputs;
        bool has_received_sequence = false;
        bool action_consumed_this_tick = false;
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
    void processHeightTileStreams(bool admit_deliveries);
    void dispatchHeightTileWork();
    void publishHeightTileResults();
    [[nodiscard]] uint32_t admitHeightTileDeliveries(PreviewStream& stream, uint32_t maximum_batches);
    void queueDepartedResidentTiles(PreviewStream& stream);
    void processInput(PlayerReplication& replication, shared::ClientInputMessage input);
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
    std::vector<SpawnPoint> m_spawn_points;
    std::vector<PlayerReplication> m_player_replications;
    struct PreviewStream final {
        struct HeightTileKeyHash final {
            [[nodiscard]] size_t operator()(shared::HeightTileKey const key) const noexcept
            {
                return static_cast<size_t>((static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32U)
                    ^ static_cast<uint32_t>(key.y));
            }
        };

        static constexpr uint32_t MAX_QUEUED_TILES = 128U;
        static constexpr uint64_t WORLD_REVISION = 1U;

        struct Delivery final {
            std::vector<shared::HeightTileKey> additions;
            std::vector<shared::HeightTileKey> removals;
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
        double background_generation_tokens = 0.0;
        std::chrono::steady_clock::time_point background_budget_updated_at =
            std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point background_generation_eligible_at =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
    };
    std::vector<PreviewStream> m_preview_streams;
    size_t m_next_preview_admission = 0U;
    size_t m_next_preview_dispatch = 0U;
    std::unique_ptr<HeightTileWorkerPool> m_height_tile_workers;
    uint64_t m_next_height_tile_token = 1;
};

} // namespace server
