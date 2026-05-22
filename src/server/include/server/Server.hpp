#pragma once

#include <core/net/Server.hpp>

namespace server {

class Server final : public core::Server {
public:
    explicit Server() : core::Server { core::Address::localhost(20040), 1, 1 } { }

    void run();
private:
    void onConnected(core::ConnectEvent const event) override;
    void onDisconnected(core::DisconnectEvent const event) override;
    void onReceived(core::ReceiveEvent event) override;
private:
    size_t m_connects = 0;
    size_t m_disconnects = 0;
};

} // namespace server
