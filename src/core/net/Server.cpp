#include <core/net/Server.hpp>

#include <core/Assert.hpp>

namespace core {

Server::~Server() {
    kick(collectConnectedClients(), std::chrono::milliseconds{ 300 }, false);
}

size_t Server::poll(std::chrono::milliseconds const total_timeout) {
    return m_host.repeatedlyPoll(
        [this](NetEvent event) { dispatchEvent(std::move(event)); },
        total_timeout
    );
}

size_t Server::kick(
    std::span<ClientId const> const clients,
    std::optional<std::chrono::milliseconds> const wait_for_graceful_disconnect_time,
    bool const generate_events
) {
    // Without graceful exit enabled just reset all the peers.
    if (!wait_for_graceful_disconnect_time.has_value()) {
        for (ClientId const client_id : clients) {
            m_host.reset(client(client_id).value());
            removeClient(client_id);
        }
        return clients.size();
    }

    // Initiate graceful disconnections.
    for (ClientId const client_id : clients) {
        m_host.disconnect(client(client_id).value());
    }

    // Wait for graceful discannect confirmations for the given timeout.
    size_t gracefully_kicked = 0;
    std::vector<bool> unkicked(clients.size(), true);
    m_host.repeatedlyPoll([&](NetEvent event) {
        auto disconnect_event = std::get_if<DisconnectEvent>(&event);
        if (!disconnect_event) {
            if (generate_events) {
                dispatchEvent(std::move(event));
            }
            return ControlFlow::Continue;
        }

        auto const client_index = findClientIndex(disconnect_event->peer).value();
        auto const client_id = m_connected_clients[client_index].first;
        if (auto it = std::find(clients.begin(), clients.end(), client_id); it != clients.end()) {
            unkicked[it - clients.begin()] = false;
            if (++gracefully_kicked == clients.size()) {
                if (generate_events) {
                    dispatchEvent(std::move(event));
                } else {
                    removeClient(*it);
                }
                return ControlFlow::Break;
            }
        }

        if (generate_events) {
            dispatchEvent(std::move(event));
        }
        return ControlFlow::Continue;
    }, *wait_for_graceful_disconnect_time);

    // Forcefully reset clients that didn't confirm graceful disconnection.
    for (size_t i = 0; i < clients.size(); ++i) {
        if (unkicked[i]) {
            m_host.reset(client(clients[i]).value());
            removeClient(clients[i]);
        }
    }

    return clients.size() - gracefully_kicked;
}

std::optional<Peer> Server::client(ClientId const id) const noexcept {
    if (auto const client_index = findClientIndex(id)) {
        return m_connected_clients[*client_index].second;
    }
    return std::nullopt;
}

std::vector<ClientId> Server::collectConnectedClients() const {
    std::vector<ClientId> result;
    result.reserve(m_connected_clients.size());
    for (auto const& [client_id, _] : m_connected_clients) {
        result.push_back(client_id);
    }
    return result;
}

void Server::dispatchEvent(NetEvent event) {
    if (auto connect_event = std::get_if<ConnectEvent>(&event)) {
        auto const& new_client = m_connected_clients.emplace_back(m_next_id++, connect_event->peer);
        onConnected(ServerConnectEvent{ 
            .client = new_client.second,
            .client_id = new_client.first,
         });
    } else if (auto disconnect_event = std::get_if<DisconnectEvent>(&event)) {
        if (auto const client_index = findClientIndex(disconnect_event->peer)) {
            auto const& client = m_connected_clients[*client_index];
            onDisconnected(ServerDisconnectEvent {
                .client = client.second,
                .client_id = client.first,
            });
            removeClient(client.first);
        } else {
            MC_ERROR("Tried to dispatch disconnect event, but no corresponding client exists in connected clients list");
        }
    } else if (auto receive_event = std::get_if<ReceiveEvent>(&event)) {
        if (auto const client_index = findClientIndex(receive_event->peer)) {
            auto const& client = m_connected_clients[*client_index];
            onReceived(ServerReceiveEvent{
                .client = client.second,
                .raw_packet = std::move(receive_event->raw_packet),
                .data = receive_event->data,
                .client_id = client.first,
                .channel_id = receive_event->channel_id,
            });
        }
    }
}

void Server::removeClient(ClientId const client) {
    std::optional index = findClientIndex(client);
    ASSERT(index.has_value(), "Kicked client not found among connected clients");
    std::swap(m_connected_clients[*index], m_connected_clients.back());
    m_connected_clients.pop_back();
}

std::optional<size_t> Server::findClientIndex(ClientId const client) const noexcept {
    for (size_t i = 0; i < m_connected_clients.size(); ++i) {
        if (m_connected_clients[i].first == client) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<size_t> Server::findClientIndex(Peer const client) const noexcept {
    for (size_t i = 0; i < m_connected_clients.size(); ++i) {
        if (m_connected_clients[i].second == client) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace coree
