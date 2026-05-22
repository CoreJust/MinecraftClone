#include <core/net/Server.hpp>

#include <core/Assert.hpp>

namespace core {

Server::~Server() {
    kick(m_connected_clients, std::chrono::milliseconds{ 500 });
}

size_t Server::poll(std::chrono::milliseconds const total_timeout) {
    return m_host.repeatedlyPoll(
        [this](NetEvent event) { dispatchEvent(std::move(event)); },
        total_timeout
    );
}

size_t Server::kick(
    std::vector<Peer> const peers,
    std::optional<std::chrono::milliseconds> const wait_for_graceful_disconnect_time
) {
    if (!wait_for_graceful_disconnect_time.has_value()) {
        for (Peer const peer : peers) {
            removeClient(peer);
            m_host.reset(peer);
        }
        return peers.size();
    }
    for (Peer const peer : peers) {
        m_host.disconnect(peer);
    }

    size_t gracefully_kicked = 0;
    std::vector<bool> unkicked(peers.size(), true);
    m_host.repeatedlyPoll([&](NetEvent event) {
        auto disconnect_event = std::get_if<DisconnectEvent>(&event);
        if (disconnect_event) {
            if (auto it = std::find(peers.begin(), peers.end(), disconnect_event->peer); it != peers.end()) {
                unkicked[it - peers.begin()] = false;
                if (++gracefully_kicked == peers.size()) {
                    dispatchEvent(std::move(event));
                    return ControlFlow::Break;
                }
            }
        }
        dispatchEvent(std::move(event));
        return ControlFlow::Continue;
    }, *wait_for_graceful_disconnect_time);
    for (size_t i = 0; i < peers.size(); ++i) {
        if (unkicked[i]) {
            removeClient(peers[i]);
            m_host.reset(peers[i]);
        }
    }

    return peers.size() - gracefully_kicked;
}

void Server::dispatchEvent(NetEvent event) {
    if (auto connect_event = std::get_if<ConnectEvent>(&event)) {
        m_connected_clients.push_back(connect_event->peer);
        onConnected(*connect_event);
    } else if (auto disconnect_event = std::get_if<DisconnectEvent>(&event)) {
        removeClient(disconnect_event->peer);
        onDisconnected(*disconnect_event);
    } else if (auto receive_event = std::get_if<ReceiveEvent>(&event)) {
        onReceived(std::move(*receive_event));
    }
}

void Server::removeClient(Peer const client) {
    auto it = std::find(m_connected_clients.begin(), m_connected_clients.end(), client);
    ASSERT(it != m_connected_clients.end(), "Kicked client not found among connected clients");
    std::swap(*it, m_connected_clients.back());
    m_connected_clients.pop_back();
}

} // namespace coree
