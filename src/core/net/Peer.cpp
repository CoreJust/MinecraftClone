#include <core/net/Peer.hpp>

#include <core/Assert.hpp>

#include <enet/enet.h>

namespace core {

uint16_t Peer::port() const noexcept {
    return m_peer->address.port;
}

std::string_view Peer::ip() const noexcept {
    static char BUFFER[64];
    ASSERT(!enet_address_get_host_ip(&m_peer->address, BUFFER, 64));
    return std::string_view{ BUFFER };
}

} // namespace core
