#include <shared/net/Message.hpp>

#include <core/common/ByteReader.hpp>
#include <core/common/ByteWriter.hpp>

#include <type_traits>

namespace shared {

namespace {

enum class MessageType : uint8_t {
    JoinRequest,
    JoinResponse,
    ClientInput,
    ServerPlayerPosition,
    ServerRemovePlayer,
};

struct MessageEncoder final {
    core::ByteWriter writer;

    std::vector<uint8_t> operator()(JoinRequestMessage const msg) {
        return writer
            .reserve(sizeof(uint8_t) + sizeof(msg.ch))
            .write(static_cast<uint8_t>(MessageType::JoinRequest))
            .write(msg.ch)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(JoinResponseMessage const msg) {
        return writer
            .reserve(sizeof(uint8_t) + sizeof(uint8_t))
            .write(static_cast<uint8_t>(MessageType::JoinResponse))
            .write(static_cast<uint8_t>(msg.accepted))
            .build()
        ;
    }

    std::vector<uint8_t> operator()(ClientInputMessage const msg) {
        return writer
            .reserve(sizeof(uint8_t) + sizeof(msg.direction.x) + sizeof(msg.direction.y))
            .write(static_cast<uint8_t>(MessageType::ClientInput))
            .write(msg.direction.x)
            .write(msg.direction.y)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(ServerPlayerPositionMessage const msg) {
        return writer
            .reserve(sizeof(uint8_t) + sizeof(msg.ch) + sizeof(msg.x) + sizeof(msg.y)
                + sizeof(msg.x_subcell) + sizeof(msg.y_subcell))
            .write(static_cast<uint8_t>(MessageType::ServerPlayerPosition))
            .write(msg.ch)
            .write(msg.x)
            .write(msg.y)
            .write(msg.x_subcell)
            .write(msg.y_subcell)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(ServerRemovePlayerMessage const msg) {
        return writer
            .reserve(sizeof(uint8_t) + sizeof(msg.ch))
            .write(static_cast<uint8_t>(MessageType::ServerRemovePlayer))
            .write(msg.ch)
            .build()
        ;
    }
};

bool isValidCharacter(char const ch) noexcept {
    return ch == '@' || ch == '#' || ch == '$' || ch == '%' || ch == '&';
}

bool isValidDirection(uint8_t const value) noexcept {
    return value != 128;
}

bool isValidMessage(Message const& message) noexcept {
    return std::visit([](auto const& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, JoinRequestMessage>) {
            return isValidCharacter(value.ch);
        } else if constexpr (std::is_same_v<T, JoinResponseMessage>) {
            return true;
        } else if constexpr (std::is_same_v<T, ClientInputMessage>) {
            return isValidDirection(value.direction.x) && isValidDirection(value.direction.y);
        } else if constexpr (std::is_same_v<T, ServerPlayerPositionMessage>) {
            return isValidCharacter(value.ch) && value.x < World::WIDTH && value.y < World::HEIGHT
                && value.x_subcell < SUBCELLS_PER_CELL && value.y_subcell < SUBCELLS_PER_CELL;
        } else {
            return isValidCharacter(value.ch);
        }
    }, message);
}


} // namespace

std::vector<uint8_t> encodeMessage(Message const message) {
    return std::visit(MessageEncoder{ core::ByteWriter{ } }, message);
}

std::optional<Message> decodeMessage(std::span<uint8_t const> const data) {
    core::ByteReader reader{ data };
    auto const type_value = reader.read<uint8_t>();
    if (!type_value) {
        return std::nullopt;
    }
    auto const type = static_cast<MessageType>(*type_value);

    std::optional<Message> message;
    switch (type) {
        case MessageType::JoinRequest: {
            auto const ch = reader.read<char>();
            if (ch) {
                message = JoinRequestMessage{ .ch = *ch };
            }
            break;
        }
        case MessageType::JoinResponse: {
            auto const accepted = reader.read<uint8_t>();
            if (!accepted || *accepted > 1) {
                return std::nullopt;
            }
            message = JoinResponseMessage{ .accepted = *accepted == 1 };
            break;
        }
        case MessageType::ClientInput: {
            auto const x = reader.read<uint8_t>();
            auto const y = reader.read<uint8_t>();
            if (x && y) {
                message = ClientInputMessage{ .direction = { .x = *x, .y = *y } };
            }
            break;
        }
        case MessageType::ServerPlayerPosition: {
            auto const ch = reader.read<char>();
            auto const x = reader.read<uint8_t>();
            auto const y = reader.read<uint8_t>();
            auto const x_subcell = reader.read<uint16_t>();
            auto const y_subcell = reader.read<uint16_t>();
            if (ch && x && y && x_subcell && y_subcell) {
                message = ServerPlayerPositionMessage{
                    .ch = *ch,
                    .x = *x,
                    .y = *y,
                    .x_subcell = *x_subcell,
                    .y_subcell = *y_subcell,
                };
            }
            break;
        }
        case MessageType::ServerRemovePlayer: {
            auto const ch = reader.read<char>();
            if (ch) {
                message = ServerRemovePlayerMessage{ .ch = *ch };
            }
            break;
        }
    default: return std::nullopt;
    }
    if (!message || reader.left() != 0 || !isValidMessage(*message)) {
        return std::nullopt;
    }
    return message;
}

} // namespace shared
