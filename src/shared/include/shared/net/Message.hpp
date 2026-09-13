#pragma once

#include <shared/world/World.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace shared {

constexpr uint8_t PROTOCOL_MAGIC = 0x4DU;
constexpr uint8_t PROTOCOL_VERSION = 1U;

struct JoinRequestMessage final {
    char ch;
    WorldMode mode = WorldMode::Flat;
    WorldConfiguration configuration = World::canonicalConfiguration();
};

struct JoinResponseMessage final {
    bool accepted;
};

struct ClientInputMessage final {
    Direction direction;
    uint32_t sequence = 0;
};

struct ServerPlayerPositionMessage final {
    char ch;
    int32_t x;
    int32_t y;
    int32_t z = 0;
    uint16_t x_subcell;
    uint16_t y_subcell;
    uint16_t z_subcell = 0;
    uint32_t acknowledged_input_sequence = 0;
    uint32_t state_revision = 0;
};

struct ServerRemovePlayerMessage final {
    char ch;
};

using Message = std::variant<
    JoinRequestMessage,
    JoinResponseMessage,
    ClientInputMessage,
    ServerPlayerPositionMessage,
    ServerRemovePlayerMessage>;

[[nodiscard]]
constexpr bool isNewerSequence(uint32_t const candidate, uint32_t const reference) noexcept
{
    return candidate != reference && candidate - reference < (uint32_t{ 1 } << 31U);
}

std::vector<uint8_t> encodeMessage(Message message);
std::optional<Message> decodeMessage(std::span<uint8_t const> data);

} // namespace shared
