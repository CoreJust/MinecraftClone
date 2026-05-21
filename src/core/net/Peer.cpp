#include <core/net/Peer.hpp>

#include <core/net/Address.hpp>
#include <core/Assert.hpp>

namespace core {

uint16_t Peer::port() const noexcept {
    return m_peer->address.port;
}

std::string_view Peer::ip() const noexcept {
    return Address::fromRaw(m_peer->address).ip();
}

} // namespace core
