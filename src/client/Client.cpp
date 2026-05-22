#include <client/Client.hpp>

#include <iostream>

namespace client {

void Client::run() {
    auto peer = m_host.connect(core::Address::localhost(20040), 1).value();
    while (true) {
        auto event = m_host.poll(std::chrono::milliseconds{ 10 });
        if (event) {
            if (std::holds_alternative<core::ConnectEvent>(*event)) {
                std::cout << "Connected to server.\n";
                break;
            } else if (std::holds_alternative<core::DisconnectEvent>(*event)) {
                std::cerr << "Connection failed.\n";
                return;
            }
        }
    }

    std::string line;
    while (pollServer()) {
        std::cout << ">>> ";
        std::getline(std::cin, line);
        if (line == "exit") {
            m_host.disconnect(peer);
            pollServer();
            break;
        }
        m_host.send(peer, std::span{ (uint8_t*)line.data(), line.size() }, 0, core::SendMode{ });
    }
}

bool Client::pollServer() {
    while (auto event = m_host.poll(std::chrono::milliseconds{ 500 })) {
        if (auto receive_event = std::get_if<core::ReceiveEvent>(&*event)) {
            std::cout << "[SERVER]: " << std::string_view{ (char*)receive_event->data.data(), receive_event->data.size() } << std::endl;
        }
        if (std::holds_alternative<core::DisconnectEvent>(*event)) {
            return false;
        }
    }
    return true;
}

} // namespace client
