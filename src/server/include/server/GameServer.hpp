#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>
#include <core/net/Server.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
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
    void onConnected(core::ServerConnectEvent const event) override;
    void onDisconnected(core::ServerDisconnectEvent const event) override;
    void onReceived(core::ServerReceiveEvent event) override;

    void send(shared::Message const message);
    void sendTo(std::optional<core::ClientId> const client_id, shared::Message message);
    [[nodiscard]]
    static std::vector<SpawnPoint> checkedSpawnPoints(std::vector<SpawnPoint> spawn_points);
private:
    shared::World m_world;
    std::vector<SpawnPoint> m_spawn_points;
    std::string m_players_moved_this_tick;
};

} // namespace server
