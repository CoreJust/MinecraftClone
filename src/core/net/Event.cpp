#include <core/net/Event.hpp>

namespace core {

void RawPacketDeleter::operator()(ENetPacket* raw_packet) noexcept {
    enet_packet_destroy(raw_packet);
}

} // namespace core
