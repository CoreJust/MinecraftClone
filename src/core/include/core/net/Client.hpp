#pragma once

#include <core/net/GenerateEvents.hpp>
#include <core/net/Host.hpp>

namespace core {

class Client {
public:
    Client(size_t const max_channels) 
        : m_host(std::nullopt, 1, max_channels)
        , m_server(std::nullopt)
    { }

    ~Client();

    // Returns true if connection succeeded.
    bool connect(
        Address const address,
        std::chrono::milliseconds const timeout);
    // Returns true if disconnection was graceful.
    bool disconnect(
        std::optional<std::chrono::milliseconds> const graceful_disconnection_timeout = std::nullopt,
        GenerateEvents const generate_events = GenerateEvents::Yes);

    size_t poll(std::chrono::milliseconds const total_timeout = std::chrono::milliseconds::zero());
    void flush() noexcept { m_host.flush(); }

    bool send(
        std::span<uint8_t const> const data,
        uint8_t const channel_id,
        SendMode const mode);

    [[nodiscard]]
    constexpr bool isConnected() const noexcept { return m_server.has_value(); }
protected:
    virtual void onDisconnected(DisconnectEvent const event) = 0;
    virtual void onReceived(ReceiveEvent event) = 0;
private:
    void dispatchEvent(NetEvent event);
private:
    Host m_host;
    std::optional<Peer> m_server;
};

} // namespace core
