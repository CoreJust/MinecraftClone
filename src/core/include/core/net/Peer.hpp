#pragma once

#include "Address.hpp"

#include <enet/enet.h>

#include <cstdint>
#include <string>

namespace core {

class Peer final {
public:
    constexpr explicit Peer(ENetPeer& peer) noexcept : m_peer(&peer) { }

    [[nodiscard]]
    constexpr bool operator==(Peer const& other) const noexcept = default;

    [[nodiscard]]
    Address address() const noexcept { return Address::fromRaw(m_peer->address); }
    [[nodiscard]]
    uint16_t port() const noexcept { return m_peer->address.port; }
    [[nodiscard]]
    std::string ip() const noexcept { return Address::fromRaw(m_peer->address).ip(); }
    
    [[nodiscard]]
    constexpr ENetPeer* raw() const noexcept { return m_peer; }
private:
    ENetPeer* m_peer;
};

} // namespace core
