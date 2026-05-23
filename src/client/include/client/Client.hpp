#pragma once

#include <core/net/Client.hpp>

namespace client {

class Client final : public core::Client {
public:
    explicit Client() : core::Client { 1 } { }

    void run();
private:
    void onDisconnected(core::DisconnectEvent const event) override;
    void onReceived(core::ReceiveEvent event) override;
private:
    bool m_running = true;
};

} // namespace client
