#include <client/GameClient.hpp>

#include <core/IO/Log.hpp>
#include <core/SpanUtils.hpp>

#include <iostream>

namespace client {

void GameClient::run(core::Address const server_address, char const ch) {
    if (!connect(server_address, std::chrono::milliseconds{ 1'000 })) {
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }
    
    send(shared::JoinRequestMessage {
        .ch = ch,
    });
    while (!m_accepted && m_running) {
        poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{ 2 });
    }

    while (m_running) {
        poll();
        render();
        send(shared::ClientInputMessage{
            .direction = input(),
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{ 40 });
    }
}

void GameClient::onDisconnected(core::DisconnectEvent const event) {
    std::cout << "[SERVER DISCONNECTED] address " << fmt::format("{}", event.peer.address()) << std::endl;
    m_running = false;
}

void GameClient::onReceived(core::ReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        MC_ERROR("Received a corrupted message");
        return;
    }

    shared::Message* msg_ptr = &*maybe_msg;
    if (auto* msg = std::get_if<shared::JoinResponseMessage>(msg_ptr)) {
        auto const [accepted] = *msg;
        m_accepted = accepted;
        if (!accepted) {
            m_running = false;
        }
    } else if (auto* msg = std::get_if<shared::ServerPlayerPositionMessage>(msg_ptr)) {
        auto const [ch, x, y] = *msg;
        if (auto p = m_world.playerByCharacter(ch)) {
            m_world.setPlayerPosition(p->id, x, y);
        } else {
            m_world.spawnPlayer(m_next_id++, ch, {{x, y}});
        }
    } else if (auto* msg = std::get_if<shared::ServerRemovePlayerMessage>(msg_ptr)) {
        auto const [ch] = *msg;
        shared::Player const p = m_world.playerByCharacter(ch).value();
        m_world.despawnPlayer(p.id);
    } else {
        MC_ERROR("Received a message unsupported by the client {}", msg_ptr->index());
    }
}

void GameClient::send(shared::Message const message) {
    std::vector const message_bytes = shared::encodeMessage(message);
    if (!core::Client::send(message_bytes, 0, core::SendMode{ core::SendMode::Reliable })) {
        MC_ERROR("Failed to send a message");
    }
}

} // namespace client
