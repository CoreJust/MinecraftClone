#include <server/GameServer.hpp>

#include <core/IO/Log.hpp>

namespace server {

void GameServer::run() {
    while (true) {
        auto const start = std::chrono::steady_clock::now();
        m_players_moved_this_tick.clear();
        poll();
        auto const tick_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        );
        if (tick_time < shared::TICK) {
            std::this_thread::sleep_for(shared::TICK - tick_time);
        }
    }
}

void GameServer::onConnected(core::ServerConnectEvent const client) {
    MC_INFO("Server: onConnected {}", client.client.address());
}

void GameServer::onDisconnected(core::ServerDisconnectEvent const client) {
    MC_INFO("Server: onDisconnected {}", client.client.address());
    send(shared::ServerRemovePlayerMessage{
        .ch = m_world.player(client.client_id)->ch,
    });
    m_world.despawnPlayer(client.client_id);
}

void GameServer::onReceived(core::ServerReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        MC_ERROR("Received a corrupted message");
        return;
    }

    shared::PlayerId const id = event.client_id;
    shared::Message* msg_ptr = &*maybe_msg;
    if (auto* msg = std::get_if<shared::JoinRequestMessage>(msg_ptr)) {
        auto const [ch] = *msg;
        if (m_world.playerExists(ch)) {
            send(shared::JoinResponseMessage{
                .accepted = false,
            });
            return;
        }
        m_world.spawnPlayer(id, ch);
        send(shared::JoinResponseMessage{
            .accepted = true,
        });

        shared::Player const p = m_world.player(id).value();
        MC_INFO("Player '{}' spawned at x {}, y {}", ch, static_cast<int>(p.x), static_cast<int>(p.y));
        send(shared::ServerPlayerPositionMessage{
            .ch = ch,
            .x = p.x,
            .y = p.y,
        });
    } else if (auto* msg = std::get_if<shared::ClientInputMessage>(msg_ptr)) {
        shared::Player p = m_world.player(id).value();
        if (m_players_moved_this_tick.contains(p.ch)) {
            return;
        }
        m_players_moved_this_tick += p.ch;

        auto const [direction] = *msg;
        if (m_world.movePlayer(id, direction)) {
            p = m_world.player(id).value();
            send(shared::ServerPlayerPositionMessage{
                .ch = p.ch,
                .x = p.x,
                .y = p.y,
            });
        }
    } else {
        MC_ERROR("Received a message unsupported by the server {}", msg_ptr->index());
    }
}

void GameServer::send(shared::Message const message) {
    std::vector const message_bytes = shared::encodeMessage(message);
    if (!core::Server::send(std::nullopt, message_bytes, 0, core::SendMode{ core::SendMode::Reliable })) {
        MC_ERROR("Failed to send a message");
    }
}

} // namespace server
