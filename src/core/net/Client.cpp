#include <core/net/Client.hpp>

#include <core/Assert.hpp>

namespace core {

Client::~Client() {
    if (isConnected()) {
        disconnect(std::chrono::milliseconds{ 500 }, false);
    }
}

bool Client::connect(
    Address const address,
    std::chrono::milliseconds const timeout
) {
    ASSERT(!m_server.has_value(), "Client cannot connect: it is already connected. Disconnect first");
    m_server = m_host.connect(address);
    if (!m_server) {
        MC_ERROR("Failed to connect to address {}", address);
        return false;
    }
    
    bool success = false;
    m_host.repeatedlyPoll([&](NetEvent event) {
        if (std::holds_alternative<core::ConnectEvent>(event)) {
            MC_INFO("Successfully connected to {}", address);
            success = true;
            return ControlFlow::Break;
        } else if (std::holds_alternative<core::DisconnectEvent>(event)) {
            MC_INFO("Failed to connect to {}", address);
            return ControlFlow::Break;
        }
        return ControlFlow::Continue;
    }, timeout);
    return success;
}

bool Client::disconnect(
    std::optional<std::chrono::milliseconds> const graceful_disconnection_timeout,
    bool const generate_events
) {
    ASSERT(m_server.has_value(), "Cannot disconnect: Client is connected to no server");

    // Without graceful exit enabled just reset all the peers.
    if (!graceful_disconnection_timeout.has_value()) {
        m_host.reset(*m_server);
        m_server = std::nullopt;
        return false;
    }

    // Initiate graceful disconnection
    m_host.disconnect(*m_server);

    // Wait for graceful discannect confirmation for the given timeout
    bool graceful = false;
    m_host.repeatedlyPoll([&](NetEvent event) {
        auto disconnect_event = std::get_if<DisconnectEvent>(&event);
        if (!disconnect_event) {
            if (generate_events) {
                dispatchEvent(std::move(event));
            }
            return ControlFlow::Continue;
        }

        graceful = true;
        if (generate_events) {
            dispatchEvent(std::move(event));
        }
        return ControlFlow::Break;
    }, *graceful_disconnection_timeout);

    // Forcefully reset server if it didn't confirm graceful disconnection
    if (!graceful) {
        m_host.reset(*m_server);
        m_server = std::nullopt;
    }

    return graceful;
}

size_t Client::poll(std::chrono::milliseconds const total_timeout) {
    return m_host.repeatedlyPoll(
        [this](NetEvent event) { dispatchEvent(std::move(event)); },
        total_timeout
    );
}

bool Client::send(
    std::span<uint8_t const> const data,
    uint8_t const channel_id,
    SendMode const mode
) {
    ASSERT(m_server.has_value(), "Cannot send data: client is not connected to server");
    return m_host.send(*m_server, data, channel_id, mode);
}

void Client::dispatchEvent(NetEvent event) {
    if (auto disconnect_event = std::get_if<DisconnectEvent>(&event)) {
        onDisconnected(*disconnect_event);
    } else if (auto receive_event = std::get_if<ReceiveEvent>(&event)) {
        onReceived(std::move(*receive_event));
    }
}

} // namespace coree
