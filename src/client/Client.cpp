#include <client/Client.hpp>

#include <core/SpanUtils.hpp>

#include <iostream>

namespace client {

void Client::run() {
    if (!connect(core::Address::localhost(20040), std::chrono::milliseconds{ 1'000 })) {
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }

    std::string line;
    while (m_running) {
        std::cout << ">>> ";
        std::getline(std::cin, line);
        if (line == "exit") {
            disconnect(std::chrono::milliseconds{ 500 });
            break;
        }
        send(core::asByteSpan(line), 0, core::SendMode{ });
        poll(std::chrono::milliseconds{ 100 });
    }
}

void Client::onDisconnected(core::DisconnectEvent const event) {
    std::cout << "[SERVER DISCONNECTED] address " << fmt::format("{}", event.peer.address()) << std::endl;
    m_running = false;
}

void Client::onReceived(core::ReceiveEvent event) {
    std::cout << "[SERVER]: " << std::string_view{ (char*)event.data.data(), event.data.size() } << std::endl;
}

} // namespace client
