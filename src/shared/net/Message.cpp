#include <shared/net/Message.hpp>

#include <type_traits>
#include <utility>

namespace shared {

namespace {

enum class MessageType : uint8_t {
    JoinRequest,
    JoinResponse,
    ClientInput,
    ServerPlayerPosition,
    ServerRemovePlayer,
};

[[nodiscard]]
constexpr bool isValidCharacter(char const ch) noexcept
{
    return ch == '@' || ch == '#' || ch == '$' || ch == '%' || ch == '&';
}

[[nodiscard]]
constexpr bool isValidDirection(uint8_t const value) noexcept
{
    return value != 128;
}

[[nodiscard]]
constexpr bool isValidMode(WorldMode const mode) noexcept
{
    return mode == WorldMode::Flat || mode == WorldMode::Flight;
}

void appendUint16(std::vector<uint8_t>& bytes, uint16_t const value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendUint32(std::vector<uint8_t>& bytes, uint32_t const value)
{
    for (uint32_t byte{ 0 }; byte < 4; ++byte) {
        bytes.push_back(static_cast<uint8_t>(value >> (byte * 8U)));
    }
}

void appendUint64(std::vector<uint8_t>& bytes, uint64_t const value)
{
    for (uint32_t byte{ 0 }; byte < 8; ++byte) {
        bytes.push_back(static_cast<uint8_t>(value >> (byte * 8U)));
    }
}

void appendInt32(std::vector<uint8_t>& bytes, int32_t const value)
{
    appendUint32(bytes, static_cast<uint32_t>(value));
}

struct MessageEncoder final {
    std::vector<uint8_t> bytes;

    void begin(MessageType const type, uint64_t const payload_size)
    {
        bytes.reserve(3U + payload_size);
        bytes.push_back(PROTOCOL_MAGIC);
        bytes.push_back(PROTOCOL_VERSION);
        bytes.push_back(static_cast<uint8_t>(type));
    }

    std::vector<uint8_t> operator()(JoinRequestMessage const& message)
    {
        begin(MessageType::JoinRequest, 25U);
        bytes.push_back(static_cast<uint8_t>(message.ch));
        bytes.push_back(static_cast<uint8_t>(message.mode));
        appendUint32(bytes, message.configuration.algorithm_version);
        appendUint64(bytes, message.configuration.seed);
        bytes.push_back(message.configuration.chunk_width);
        bytes.push_back(message.configuration.chunk_height);
        bytes.push_back(message.configuration.chunk_depth);
        appendUint64(bytes, message.configuration.chunk_content_digest);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(JoinResponseMessage const& message)
    {
        begin(MessageType::JoinResponse, 1U);
        bytes.push_back(static_cast<uint8_t>(message.accepted));
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ClientInputMessage const& message)
    {
        begin(MessageType::ClientInput, 7U);
        bytes.push_back(message.direction.x);
        bytes.push_back(message.direction.y);
        bytes.push_back(message.direction.z);
        appendUint32(bytes, message.sequence);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerPlayerPositionMessage const& message)
    {
        begin(MessageType::ServerPlayerPosition, 27U);
        bytes.push_back(static_cast<uint8_t>(message.ch));
        appendInt32(bytes, message.x);
        appendInt32(bytes, message.y);
        appendInt32(bytes, message.z);
        appendUint16(bytes, message.x_subcell);
        appendUint16(bytes, message.y_subcell);
        appendUint16(bytes, message.z_subcell);
        appendUint32(bytes, message.acknowledged_input_sequence);
        appendUint32(bytes, message.state_revision);
        return std::move(bytes);
    }

    std::vector<uint8_t> operator()(ServerRemovePlayerMessage const& message)
    {
        begin(MessageType::ServerRemovePlayer, 1U);
        bytes.push_back(static_cast<uint8_t>(message.ch));
        return std::move(bytes);
    }
};

class MessageReader final {
public:
    explicit MessageReader(std::span<uint8_t const> const data) noexcept
        : m_data(data)
    {
    }

    [[nodiscard]]
    std::optional<uint8_t> readUint8() noexcept
    {
        if (m_position == static_cast<uint64_t>(m_data.size())) {
            return std::nullopt;
        }
        return m_data.data()[m_position++];
    }

    [[nodiscard]]
    std::optional<uint16_t> readUint16() noexcept
    {
        auto const low = readUint8();
        auto const high = readUint8();
        if (!low || !high) {
            return std::nullopt;
        }
        return static_cast<uint16_t>(*low) | static_cast<uint16_t>(*high) << 8U;
    }

    [[nodiscard]]
    std::optional<uint32_t> readUint32() noexcept
    {
        uint32_t result = 0;
        for (uint32_t byte{ 0 }; byte < 4; ++byte) {
            auto const value = readUint8();
            if (!value) {
                return std::nullopt;
            }
            result |= static_cast<uint32_t>(*value) << (byte * 8U);
        }
        return result;
    }

    [[nodiscard]]
    std::optional<uint64_t> readUint64() noexcept
    {
        uint64_t result = 0;
        for (uint32_t byte{ 0 }; byte < 8; ++byte) {
            auto const value = readUint8();
            if (!value) {
                return std::nullopt;
            }
            result |= static_cast<uint64_t>(*value) << (byte * 8U);
        }
        return result;
    }

    [[nodiscard]]
    std::optional<int32_t> readInt32() noexcept
    {
        auto const value = readUint32();
        if (!value) {
            return std::nullopt;
        }
        return static_cast<int32_t>(*value);
    }

    [[nodiscard]]
    bool atEnd() const noexcept
    {
        return m_position == static_cast<uint64_t>(m_data.size());
    }
private:
    std::span<uint8_t const> m_data;
    uint64_t m_position = 0;
};

[[nodiscard]]
std::optional<WorldConfiguration> readConfiguration(MessageReader& reader) noexcept
{
    auto const algorithm_version = reader.readUint32();
    auto const seed = reader.readUint64();
    auto const chunk_width = reader.readUint8();
    auto const chunk_height = reader.readUint8();
    auto const chunk_depth = reader.readUint8();
    auto const chunk_content_digest = reader.readUint64();
    if (!algorithm_version || !seed || !chunk_width || !chunk_height || !chunk_depth || !chunk_content_digest) {
        return std::nullopt;
    }
    return WorldConfiguration{
        .algorithm_version = *algorithm_version,
        .seed = *seed,
        .chunk_width = *chunk_width,
        .chunk_height = *chunk_height,
        .chunk_depth = *chunk_depth,
        .chunk_content_digest = *chunk_content_digest,
    };
}

[[nodiscard]]
bool isValidMessage(Message const& message) noexcept
{
    return std::visit([](auto const& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, JoinRequestMessage>) {
            return isValidCharacter(value.ch) && isValidMode(value.mode)
                && isValidWorldConfiguration(value.configuration);
        } else if constexpr (std::is_same_v<Value, JoinResponseMessage>) {
            return true;
        } else if constexpr (std::is_same_v<Value, ClientInputMessage>) {
            return isValidDirection(value.direction.x) && isValidDirection(value.direction.y)
                && isValidDirection(value.direction.z);
        } else if constexpr (std::is_same_v<Value, ServerPlayerPositionMessage>) {
            return isValidCharacter(value.ch) && World::isFlightPositionInBounds(PlayerPosition{
                .x = value.x,
                .y = value.y,
                .z = value.z,
                .x_subcell = value.x_subcell,
                .y_subcell = value.y_subcell,
                .z_subcell = value.z_subcell,
            });
        } else {
            return isValidCharacter(value.ch);
        }
    }, message);
}

} // namespace

std::vector<uint8_t> encodeMessage(Message message)
{
    return std::visit(MessageEncoder{}, std::move(message));
}

std::optional<Message> decodeMessage(std::span<uint8_t const> const data)
{
    MessageReader reader{ data };
    auto const magic = reader.readUint8();
    auto const version = reader.readUint8();
    auto const type_value = reader.readUint8();
    if (!magic || !version || !type_value || *magic != PROTOCOL_MAGIC || *version != PROTOCOL_VERSION) {
        return std::nullopt;
    }

    std::optional<Message> message;
    switch (static_cast<MessageType>(*type_value)) {
        case MessageType::JoinRequest: {
            auto const character = reader.readUint8();
            auto const mode = reader.readUint8();
            auto const configuration = readConfiguration(reader);
            if (character && mode && configuration) {
                message = JoinRequestMessage{
                    .ch = static_cast<char>(*character),
                    .mode = static_cast<WorldMode>(*mode),
                    .configuration = *configuration,
                };
            }
            break;
        }
        case MessageType::JoinResponse: {
            auto const accepted = reader.readUint8();
            if (!accepted || *accepted > 1) {
                return std::nullopt;
            }
            message = JoinResponseMessage{ .accepted = *accepted == 1 };
            break;
        }
        case MessageType::ClientInput: {
            auto const x = reader.readUint8();
            auto const y = reader.readUint8();
            auto const z = reader.readUint8();
            auto const sequence = reader.readUint32();
            if (x && y && z && sequence) {
                message = ClientInputMessage{
                    .direction = { .x = *x, .y = *y, .z = *z },
                    .sequence = *sequence,
                };
            }
            break;
        }
        case MessageType::ServerPlayerPosition: {
            auto const character = reader.readUint8();
            auto const x = reader.readInt32();
            auto const y = reader.readInt32();
            auto const z = reader.readInt32();
            auto const x_subcell = reader.readUint16();
            auto const y_subcell = reader.readUint16();
            auto const z_subcell = reader.readUint16();
            auto const acknowledged_input_sequence = reader.readUint32();
            auto const state_revision = reader.readUint32();
            if (character && x && y && z && x_subcell && y_subcell && z_subcell
                && acknowledged_input_sequence && state_revision) {
                message = ServerPlayerPositionMessage{
                    .ch = static_cast<char>(*character),
                    .x = *x,
                    .y = *y,
                    .z = *z,
                    .x_subcell = *x_subcell,
                    .y_subcell = *y_subcell,
                    .z_subcell = *z_subcell,
                    .acknowledged_input_sequence = *acknowledged_input_sequence,
                    .state_revision = *state_revision,
                };
            }
            break;
        }
        case MessageType::ServerRemovePlayer: {
            auto const character = reader.readUint8();
            if (character) {
                message = ServerRemovePlayerMessage{ .ch = static_cast<char>(*character) };
            }
            break;
        }
        default: return std::nullopt;
    }
    if (!message || !reader.atEnd() || !isValidMessage(*message)) {
        return std::nullopt;
    }
    return message;
}

} // namespace shared
