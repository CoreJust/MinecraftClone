#include <server/Server.hpp>

#include <core/IO/Log.hpp>

namespace server {

void Server::run() {
    while (true) {
        auto event = m_host.poll(std::chrono::milliseconds{ 100 });
        if (!event) {
            continue;
        }
        if (auto receive_event = std::get_if<core::ReceiveEvent>(&*event)) {
            std::string msg{ (char*)receive_event->data.data(), receive_event->data.size() };
            msg += "? I don't think so.";
            m_host.send(std::nullopt, std::span{ (uint8_t*)msg.data(), msg.size() }, 0, core::SendMode{ });
        } else if (std::holds_alternative<core::ConnectEvent>(*event)) {
            MC_INFO("Server: client connected");
        } else if (std::holds_alternative<core::DisconnectEvent>(*event)) {
            MC_INFO("Server: client disconnected");
            break;
        }
    }
}

} // namespace server
