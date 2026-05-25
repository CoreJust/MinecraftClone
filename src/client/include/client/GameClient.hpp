#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>
#include <core/net/Client.hpp>

namespace client {

class GameClient : public core::Client {
public:
    explicit GameClient() : core::Client { 1 } { }

    void run(core::Address const server_address, char const ch);
protected:
    virtual shared::Direction input() = 0;
    virtual void render() = 0;

    void send(shared::Message const message);
private:
    void onDisconnected(core::DisconnectEvent const event) override;
    void onReceived(core::ReceiveEvent event) override;
protected:
    shared::World m_world;
    shared::PlayerId m_next_id = 0;
    bool m_running = true;
    bool m_accepted = false;
};

} // namespace client
