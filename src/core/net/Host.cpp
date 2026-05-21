#include <core/net/Host.hpp>

#include <core/Assert.hpp>

namespace core {

Host::Host(
    std::optional<Address> const address,
    size_t const max_connections,
    size_t const max_channels
) {
    if (address.has_value()) {
        m_host = enet_host_create(&address->raw(), max_connections, max_channels, 0, 0);
    } else {
        m_host = enet_host_create(NULL, max_connections, max_channels, 0, 0);
    }
    if (!m_host) {
        MC_ERROR("Failed to create ENet host");
    } else {
        MC_INFO(
            "Created host at {} with {} max connections and {} max channels",
            address, max_connections, max_channels);
    }
}

Host::~Host() {
    if (m_host) {
        enet_host_destroy(m_host);
        m_host = nullptr;
        MC_INFO("Destroyed host");
    }
}

std::optional<NetEvent> Host::poll(std::chrono::milliseconds const timeout) {
    ASSERT(m_host);

    ENetEvent raw_event;
    int const host_service_result = enet_host_service(m_host, &raw_event, static_cast<enet_uint32>(timeout.count()));
    if (host_service_result < 0) {
        MC_DEBUG("Error occured during enet_host_service, returned value is {}", host_service_result);
        return std::nullopt;
    }

    if (host_service_result == 0 || raw_event.type == ENET_EVENT_TYPE_NONE) {
        return std::nullopt;
    }

    switch (raw_event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            ASSERT(raw_event.peer != nullptr);
            return ConnectEvent{ .peer = Peer{ *raw_event.peer } };
        case ENET_EVENT_TYPE_DISCONNECT:
            ASSERT(raw_event.peer != nullptr);
            return DisconnectEvent{ .peer = Peer{ *raw_event.peer } };
        case ENET_EVENT_TYPE_RECEIVE:
            ASSERT(raw_event.peer != nullptr && raw_event.packet != nullptr && raw_event.packet->data != nullptr);
            return ReceiveEvent{
                .peer = Peer{ *raw_event.peer },
                .channel_id = raw_event.channelID,
                .raw_packet = RawPacket{ raw_event.packet },
                .data = std::span{ raw_event.packet->data, raw_event.packet->dataLength },
            };
        default:
            ASSERT("Unreachable");
            return std::nullopt;
    }
}

void Host::flush() noexcept {
    ASSERT(m_host);
    enet_host_flush(m_host);
}

bool Host::send(
    std::optional<Peer> const peer,
    std::span<uint8_t const> const data,
    uint8_t const channel_id,
    SendMode const mode
) {
    if (!mode.isValid()) {
        MC_ERROR("SendMode has an invalid configuration");
        return false;
    }

    ENetPacket* packet = enet_packet_create(data.data(), data.size_bytes(), mode.raw());
    if (!packet) {
        MC_ERROR("Failed to create packet");
        return false;
    }

    if (!peer.has_value()) { // Send to all
        ASSERT(m_host);
        enet_host_broadcast(m_host, channel_id, packet);
        return true;
    }

    if (enet_peer_send(peer->raw(), channel_id, packet) < 0) {
        MC_ERROR("Failed to send a packet to peer");
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

std::optional<Peer> Host::connect(Address const address, size_t const channels_count) {
    if (ENetPeer *peer = enet_host_connect(m_host, &address.raw(), channels_count, 0)) {
        MC_INFO(
            "Initiated connection to {} with {} channels",
            address, channels_count);
        return Peer{ *peer };
    }
    MC_ERROR(
        "Failed to connect to {} with {} channels",
        address, channels_count);
    return std::nullopt;
}

void Host::disconnect(Peer const peer) noexcept {
    enet_peer_disconnect(peer.raw(), 0);
    MC_INFO("Initiated disconnection to peer {}:{}", peer.ip(), peer.port());
}

void Host::reset(Peer const peer) noexcept {
    enet_peer_reset(peer.raw());
    MC_INFO("Reset peer {}:{}", peer.ip(), peer.port());
}

uint16_t Host::port() const noexcept {
    ASSERT(m_host);
    return m_host->address.port;
}

std::string Host::ip() const noexcept {
    ASSERT(m_host);
    return Address::fromRaw(m_host->address).ip();
}

} // namespace core
