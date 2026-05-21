#pragma once

#include "Peer.hpp"

#include <memory>
#include <span>
#include <variant>

using ENetPacket = struct _ENetPacket;

namespace core {

struct RawPacketDeleter final {
    void operator()(ENetPacket* raw_packet) noexcept;
};

using RawPacket = std::unique_ptr<ENetPacket, RawPacketDeleter>;

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

} // namespace core
