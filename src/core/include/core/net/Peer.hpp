#pragma once

#include <cstdint>
#include <string>

using ENetPeer = struct _ENetPeer;

namespace core {

class Peer final {
public:
    constexpr explicit Peer(ENetPeer& peer) noexcept : m_peer(&peer) { }

    [[nodiscard]]
    uint16_t port() const noexcept;
    [[nodiscard]]
    std::string_view ip() const noexcept;
    
    [[nodiscard]]
    constexpr ENetPeer* raw() const noexcept { return m_peer; }
private:
    ENetPeer* m_peer;
};

} // namespace core
