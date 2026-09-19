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
    )
        : core::Server{ core::Address::localhost(port), 4, 2 }
        , m_world{ world_mode, configuration }
        , m_spawn_points{ checkedSpawnPoints(std::move(spawn_points), world_mode) }
    { }

    [[nodiscard]]
    static std::expected<std::vector<SpawnPoint>, std::string> validateSpawnPoints(
        std::vector<SpawnPoint> spawn_points,
        shared::WorldMode world_mode = shared::WorldMode::Flat
    );

    [[nodiscard]]
    static std::chrono::milliseconds fixedTickDelay(std::chrono::milliseconds elapsed) noexcept;

    void run();
    void run(std::atomic_bool const& stop_requested);
    [[nodiscard]]
    uint64_t tick(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
private:
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
    void sendPreviewTo(std::optional<core::ClientId> const client_id, shared::Message message);
    void startPreviewSet(core::ClientId client_id, shared::Player player);
    void processPendingPreviewSet();
    void processPreviewStreams();
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
    std::deque<std::pair<core::ClientId, shared::Player>> m_pending_preview_sets;
    struct PreviewStream final {
        static constexpr uint32_t PREVIEW_RADIUS = 14U;
        static constexpr uint32_t PREVIEW_DIAMETER = PREVIEW_RADIUS * 2U + 1U;
        static constexpr uint32_t MAX_PREVIEW_CHUNKS = PREVIEW_DIAMETER * PREVIEW_DIAMETER;
        static constexpr uint64_t WORLD_REVISION = 1U;

        PreviewStream(core::ClientId client_id, shared::Player player)
            : client_id(client_id)
            , player(player)
        { }

        core::ClientId client_id;
        shared::Player player;
        shared::WorldGenerationScheduler scheduler{MAX_PREVIEW_CHUNKS};
        std::vector<shared::PreviewChunkKey> coarse_keys;
        uint32_t coarse_completed = 0U;
        uint32_t sent_chunks = 0U;
    };
    std::vector<PreviewStream> m_preview_streams;
    shared::TerrainGenerator m_preview_generator;
    uint64_t m_next_preview_token = 1;
};

} // namespace server
