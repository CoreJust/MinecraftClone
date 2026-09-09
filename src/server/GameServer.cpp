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
    CORE_INFO("Server: onConnected {}", client.client.address());
}

void GameServer::onDisconnected(core::ServerDisconnectEvent const client) {
    CORE_INFO("Server: onDisconnected {}", client.client.address());
    auto const player = m_world.player(client.client_id);
    if (!player.has_value()) {
        return;
    }
    send(shared::ServerRemovePlayerMessage{
        .ch = player->ch,
    });
    m_world.despawnPlayer(client.client_id);
}

void GameServer::onReceived(core::ServerReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        CORE_ERROR("Received a corrupted message");
        return;
    }

    shared::PlayerId const id = event.client_id;
    shared::Message* msg_ptr = &*maybe_msg;
    if (auto* msg = std::get_if<shared::JoinRequestMessage>(msg_ptr)) {
        auto const [ch] = *msg;
        if (m_world.player(id) || m_world.playerExists(ch)) {
            sendTo(id, shared::JoinResponseMessage{
                .accepted = false,
            });
            return;
        }
        m_world.spawnPlayer(id, ch);
        sendTo(id, shared::JoinResponseMessage{
            .accepted = true,
        });

        shared::Player const p = m_world.player(id).value();
        CORE_INFO("Player '{}' spawned at x {}, y {}", ch, static_cast<int>(p.x), static_cast<int>(p.y));
        for (shared::Player const& player : m_world.players()) {
            shared::ServerPlayerPositionMessage const position{
                .ch = player.ch,
                .x = player.x,
                .y = player.y,
            };
            if (player.id == id) {
                send(position);
            } else {
                sendTo(id, position);
            }
        }
    } else if (auto* msg = std::get_if<shared::ClientInputMessage>(msg_ptr)) {
        auto const player = m_world.player(id);
        if (!player || m_players_moved_this_tick.contains(player->ch)) {
            return;
        }
        auto const [direction] = *msg;
        if (direction.x == 0 && direction.y == 0) {
            return;
        }
        m_players_moved_this_tick += player->ch;
        if (m_world.movePlayer(id, direction)) {
            auto const moved_player = m_world.player(id);
            if (moved_player) {
                send(shared::ServerPlayerPositionMessage{
                    .ch = moved_player->ch,
                    .x = moved_player->x,
                    .y = moved_player->y,
                });
            }
        }
    } else {
        CORE_ERROR("Received a message unsupported by the server {}", msg_ptr->index());
    }
}

void GameServer::send(shared::Message const message) {
    sendTo(std::nullopt, std::move(message));
}

void GameServer::sendTo(std::optional<core::ClientId> const client_id, shared::Message message) {
    std::vector const message_bytes = shared::encodeMessage(std::move(message));
    std::optional<core::Peer> peer;
    if (client_id.has_value()) {
        peer = client(*client_id);
        if (!peer) {
            CORE_ERROR("Cannot send a message to disconnected client {}", *client_id);
            return;
        }
    }
    if (!core::Server::send(peer, message_bytes, 0, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a message");
    }
}

} // namespace server
