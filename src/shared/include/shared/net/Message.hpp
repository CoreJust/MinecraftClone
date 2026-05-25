#pragma once

#include <shared/world/World.hpp>

#include <cstdint>
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
};

struct ServerPlayerPositionMessage final {
    char ch;
    uint8_t x;
    uint8_t y;
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

std::vector<uint8_t> encodeMessage(Message const message);
std::optional<Message> decodeMessage(std::span<uint8_t> const data);

} // namespace shared
