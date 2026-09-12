#pragma once

#include <shared/world/World.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace shared {

struct JoinRequestMessage final {
    char ch;
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
    uint8_t x;
    uint8_t y;
    uint16_t x_subcell;
    uint16_t y_subcell;
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

std::vector<uint8_t> encodeMessage(Message const message);
std::optional<Message> decodeMessage(std::span<uint8_t const> const data);

} // namespace shared
