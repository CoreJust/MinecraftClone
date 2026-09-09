#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>
#include <core/net/Server.hpp>

namespace server {

class GameServer final : public core::Server {
public:
    explicit GameServer(uint16_t const port = 20'040)
        : core::Server{ core::Address::localhost(port), 4, 1 }
    { }

    void run();
private:
    void onConnected(core::ServerConnectEvent const event) override;
    void onDisconnected(core::ServerDisconnectEvent const event) override;
    void onReceived(core::ServerReceiveEvent event) override;

    void send(shared::Message const message);
    void sendTo(std::optional<core::ClientId> const client_id, shared::Message message);
private:
    shared::World m_world;
    std::string m_players_moved_this_tick;
};

} // namespace server
