#pragma once

#include "Peer.hpp"

#include <enet/enet.h>

#include <memory>
#include <span>
#include <variant>

namespace core {

struct RawPacketDeleter final {
    void operator()(ENetPacket* raw_packet) noexcept;
};

using RawPacket = std::unique_ptr<ENetPacket, RawPacketDeleter>;
using ClientId = uint32_t;

struct ConnectEvent final {
    Peer peer;
};

struct DisconnectEvent final {
    Peer peer;
};

struct ReceiveEvent final {
    Peer peer;
    size_t channel_id;
    RawPacket raw_packet;
    std::span<uint8_t> data;
};

using NetEvent = std::variant<ConnectEvent, DisconnectEvent, ReceiveEvent>;

struct ServerConnectEvent final {
    Peer client;
    ClientId client_id;
};

struct ServerDisconnectEvent final {
    Peer client;
    ClientId client_id;
};

struct ServerReceiveEvent final {
    Peer client;
    ClientId client_id;
    size_t channel_id;
    RawPacket raw_packet;
    std::span<uint8_t> data;
};

} // namespace core
