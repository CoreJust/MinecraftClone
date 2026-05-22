#pragma once

#include "Host.hpp"

#include <vector>

namespace core {

class Server {
public:
    Server(
        Address const address,
        size_t const max_connections,
        size_t const max_channels
    ) : m_host(address, max_connections, max_channels) { }

    ~Server();

    size_t poll(std::chrono::milliseconds const total_timeout = std::chrono::milliseconds::zero());
    void flush() noexcept { m_host.flush(); }

    bool send(
        std::optional<Peer> const peer,
        std::span<uint8_t const> const data,
        uint8_t const channel_id,
        SendMode const mode
    ) {
        return m_host.send(peer, data, channel_id, mode);
    }

    // Returns number of ungraceful kicks
    size_t kick(
        std::vector<Peer> const peers, // Intentionally copied to avoid dangling references in the process.
        std::optional<std::chrono::milliseconds> const wait_for_graceful_disconnect_time = std::nullopt
    );
    // Returns true if kick was graceful
    bool kick(
        Peer const peer,
        std::optional<std::chrono::milliseconds> const wait_for_graceful_disconnect_time = std::nullopt
    ) {
        return kick(std::vector{ peer }, wait_for_graceful_disconnect_time) == 0;
    }

    [[nodiscard]]
    uint16_t port() const noexcept { return m_host.port(); }
    [[nodiscard]]
    std::string ip() const noexcept { return m_host.ip(); }

    [[nodiscard]]
    constexpr bool isValid() const noexcept { return m_host.isValid(); }

    // Should not be saved since any disconnection invalidates it.
    [[nodiscard]]
    constexpr std::vector<Peer> const& connectedClients() const noexcept { return m_connected_clients; }
    [[nodiscard]]
    constexpr auto& host(this auto&& self) noexcept { return self.m_host; }
protected:
    virtual void onConnected(ConnectEvent const event) = 0;
    virtual void onDisconnected(DisconnectEvent const event) = 0;
    virtual void onReceived(ReceiveEvent event) = 0;
private:
    void dispatchEvent(NetEvent event);
    void removeClient(Peer const client);
private:
    std::vector<Peer> m_connected_clients;
    Host m_host;
};

} // namespace core
