#include <core/net/Event.hpp>

#include <enet/enet.h>

namespace core {

void RawPacketDeleter::operator()(ENetPacket* raw_packet) noexcept {
    enet_packet_destroy(raw_packet);
}

} // namespace core
