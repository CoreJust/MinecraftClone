#include <server/Server.hpp>

#include <core/IO/Log.hpp>

namespace server {

void Server::run() {
    while (!m_connects || m_disconnects < m_connects) {
        poll(std::chrono::milliseconds{ 100 });
    }
}

void Server::onConnected(core::ConnectEvent const event) {
    MC_INFO("Server: onConnected {}", event.peer.address());
    ++m_connects;
}

void Server::onDisconnected(core::DisconnectEvent const event) {
    MC_INFO("Server: onDisconnected {}", event.peer.address());
    ++m_disconnects;
}

void Server::onReceived(core::ReceiveEvent event) {
    std::string msg{ (char*)event.data.data(), event.data.size() };
    MC_INFO("Server: onReceived (channel {}): {}", event.channel_id, msg);

    if (msg == "stop me") {
        if (kick(event.peer, std::chrono::milliseconds{ 5'000 })) {
            MC_INFO("Kicked the client gracefully");
        } else {
            MC_INFO("Failed to kick the client gracefully");
        }
        return;
    }

    msg += "? I don't think so.";
    send(event.peer, std::span{ (uint8_t*)msg.data(), msg.size() }, 0, core::SendMode{ });
}

} // namespace server
