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
            .reserve(sizeof(MessageType) + sizeof(msg))
            .write(MessageType::JoinRequest)
            .write(msg)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(JoinResponseMessage const msg) {
        return writer
            .reserve(sizeof(MessageType) + sizeof(msg))
            .write(MessageType::JoinResponse)
            .write(msg)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(ClientInputMessage const msg) {
        return writer
            .reserve(sizeof(MessageType) + sizeof(msg))
            .write(MessageType::ClientInput)
            .write(msg)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(ServerPlayerPositionMessage const msg) {
        return writer
            .reserve(sizeof(MessageType) + sizeof(msg))
            .write(MessageType::ServerPlayerPosition)
            .write(msg)
            .build()
        ;
    }

    std::vector<uint8_t> operator()(ServerRemovePlayerMessage const msg) {
        return writer
            .reserve(sizeof(MessageType) + sizeof(msg))
            .write(MessageType::ServerRemovePlayer)
            .write(msg)
            .build()
        ;
    }
};

bool isValidCharacter(char const ch) noexcept {
    return ch == '@' || ch == '#' || ch == '$' || ch == '%' || ch == '&';
}

bool isValidDirection(uint8_t const value) noexcept {
    return value == 0 || value == 1 || value == 255;
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
            return isValidCharacter(value.ch) && value.x < World::WIDTH && value.y < World::HEIGHT;
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
    auto const type = reader.read<MessageType>();
    if (!type) {
        return std::nullopt;
    }

    std::optional<Message> message;
    switch (*type) {
        case MessageType::JoinRequest:          message = reader.read<JoinRequestMessage>(); break;
        case MessageType::JoinResponse: {
            auto const accepted = reader.read<uint8_t>();
            if (!accepted || *accepted > 1) {
                return std::nullopt;
            }
            message = JoinResponseMessage{ .accepted = *accepted == 1 };
            break;
        }
        case MessageType::ClientInput:          message = reader.read<ClientInputMessage>(); break;
        case MessageType::ServerPlayerPosition: message = reader.read<ServerPlayerPositionMessage>(); break;
        case MessageType::ServerRemovePlayer:   message = reader.read<ServerRemovePlayerMessage>(); break;
    default: return std::nullopt;
    }
    if (!message || reader.left() != 0 || !isValidMessage(*message)) {
        return std::nullopt;
    }
    return message;
}

} // namespace shared
