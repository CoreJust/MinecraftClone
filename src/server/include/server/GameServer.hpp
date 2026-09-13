#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>
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
        uint8_t x;
        uint8_t y;
    };

    explicit GameServer(
        uint16_t port = 20'040,
        std::vector<SpawnPoint> spawn_points = { }
    )
        : core::Server{ core::Address::localhost(port), 4, 1 }
        , m_spawn_points{ checkedSpawnPoints(std::move(spawn_points)) }
    { }

    [[nodiscard]]
    static std::expected<std::vector<SpawnPoint>, std::string> validateSpawnPoints(
        std::vector<SpawnPoint> spawn_points
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
    void processInput(PlayerReplication& replication, shared::ClientInputMessage input);
    [[nodiscard]]
    PlayerReplication* playerReplication(shared::PlayerId id) noexcept;
    [[nodiscard]]
    shared::ServerPlayerPositionMessage playerPositionMessage(
        shared::Player const& player,
        PlayerReplication const& replication
    ) const noexcept;
    [[nodiscard]]
    static std::vector<SpawnPoint> checkedSpawnPoints(std::vector<SpawnPoint> spawn_points);
private:
    shared::World m_world;
    std::vector<SpawnPoint> m_spawn_points;
    std::vector<PlayerReplication> m_player_replications;
};

} // namespace server
