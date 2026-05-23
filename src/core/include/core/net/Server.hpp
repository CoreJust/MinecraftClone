#pragma once

#include "Host.hpp"

#include <core/SpanUtils.hpp>

#include <optional>
#include <span>
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

    // Returns number of ungraceful kicks.
    size_t kick(
        std::span<ClientId const> const clients,
        std::optional<std::chrono::milliseconds> const wait_for_graceful_disconnect_time = std::nullopt,
        bool const generate_events = true
    );
    // Returns true if kick was graceful.
    bool kick(
        ClientId const client,
        std::optional<std::chrono::milliseconds> const wait_for_graceful_disconnect_time = std::nullopt,
        bool const generate_events = true
    ) {
        return kick(unitSpan(client), wait_for_graceful_disconnect_time, generate_events) == 0;
    }

    [[nodiscard]]
    std::optional<Peer> client(ClientId const id) const noexcept;
    [[nodiscard]]
    std::vector<ClientId> collectConnectedClients() const;

    [[nodiscard]]
    uint16_t port() const noexcept { return m_host.port(); }
    [[nodiscard]]
    std::string ip() const noexcept { return m_host.ip(); }

    [[nodiscard]]
    constexpr bool isValid() const noexcept { return m_host.isValid(); }

    [[nodiscard]]
    constexpr auto& host(this auto&& self) noexcept { return self.m_host; }
protected:
    virtual void onConnected(ServerConnectEvent const event) = 0;
    virtual void onDisconnected(ServerDisconnectEvent const event) = 0;
    virtual void onReceived(ServerReceiveEvent event) = 0;
private:
    void dispatchEvent(NetEvent event);
    void removeClient(ClientId const client);
    std::optional<size_t> findClientIndex(ClientId const client) const noexcept;
    std::optional<size_t> findClientIndex(Peer const client) const noexcept;
private:
    std::vector<std::pair<ClientId, Peer>> m_connected_clients;
    Host m_host;
    ClientId m_next_id = 0;
};

} // namespace core
