#include <server/Server.hpp>

#include <core/IO/Log.hpp>

namespace server {

void Server::run() {
    while (!m_connects || m_disconnects < m_connects) {
        poll(std::chrono::milliseconds{ 100 });
    }
}

void Server::onConnected(core::ServerConnectEvent const client) {
    MC_INFO("Server: onConnected {}", client.client.address());
    ++m_connects;
}

void Server::onDisconnected(core::ServerDisconnectEvent const client) {
    MC_INFO("Server: onDisconnected {}", client.client.address());
    ++m_disconnects;
}

void Server::onReceived(core::ServerReceiveEvent event) {
    std::string msg{ (char*)event.data.data(), event.data.size() };
    MC_INFO("Server: onReceived (channel {}): {}", event.channel_id, msg);

    if (msg == "stop me") {
        if (kick(event.client_id, std::chrono::milliseconds{ 5'000 })) {
            MC_INFO("Kicked the client gracefully");
        } else {
            MC_INFO("Failed to kick the client gracefully");
        }
        return;
    }

    msg += "? I don't think so.";
    send(event.client, core::asByteSpan(msg), 0, core::SendMode{ });
}

} // namespace server
