#include <shared/net/Message.hpp>

#include <core/common/ByteReader.hpp>
#include <core/common/ByteWriter.hpp>

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

} // namespace

std::vector<uint8_t> encodeMessage(Message const message) {
    return std::visit(MessageEncoder{ core::ByteWriter{ } }, message);
}

std::optional<Message> decodeMessage(std::span<uint8_t> const data) {
    core::ByteReader reader{ data };
    auto const type = reader.read<MessageType>();
    if (!type) {
        return std::nullopt;
    }
    switch (*type) {
        case MessageType::JoinRequest:          return reader.read<JoinRequestMessage>();
        case MessageType::JoinResponse:         return reader.read<JoinResponseMessage>();
        case MessageType::ClientInput:          return reader.read<ClientInputMessage>();
        case MessageType::ServerPlayerPosition: return reader.read<ServerPlayerPositionMessage>();
        case MessageType::ServerRemovePlayer:   return reader.read<ServerRemovePlayerMessage>();
    default: return std::nullopt;
    }
}

} // namespace shared
