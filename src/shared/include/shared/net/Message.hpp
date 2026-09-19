#pragma once

#include <shared/world/World.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace shared {

constexpr uint8_t PROTOCOL_MAGIC = 0x4DU;
constexpr uint8_t PROTOCOL_VERSION = 3U;
constexpr uint8_t GAME_CHANNEL = 0U;
constexpr uint8_t PREVIEW_CHANNEL = 1U;
constexpr uint32_t PREVIEW_MAX_PAYLOAD_BYTES = 65'536U;

struct PreviewChunkKey final {
    int32_t x;
    int32_t y;
    int32_t z;

    constexpr bool operator==(PreviewChunkKey const&) const noexcept = default;
};

enum class PreviewLevel : uint8_t {
    Coarse,
    Final,
};

struct JoinRequestMessage final {
    char ch;
    WorldMode mode = WorldMode::Flat;
    WorldConfiguration configuration = World::canonicalConfiguration();
    bool wants_previews = false;
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

struct ServerPreviewDescriptorMessage final {
    WorldConfiguration configuration = World::canonicalConfiguration();
    uint64_t world_revision = 1;
    uint32_t max_preview_chunks = 64;
    uint32_t max_preview_bytes = PREVIEW_MAX_PAYLOAD_BYTES;
};

struct ServerWorldRevisionMessage final {
    uint64_t world_revision = 1;
};

struct ServerChunkPreviewMessage final {
    PreviewChunkKey key{};
    uint64_t revision = 1;
    uint64_t token = 0;
    PreviewLevel level = PreviewLevel::Coarse;
    uint32_t length = 0;
    uint64_t digest = 0;
    std::vector<uint8_t> bytes;
};

using PreviewDescriptorMessage = ServerPreviewDescriptorMessage;
using WorldRevisionMessage = ServerWorldRevisionMessage;
using ChunkPreviewMessage = ServerChunkPreviewMessage;

using Message = std::variant<
    JoinRequestMessage,
    JoinResponseMessage,
    ClientInputMessage,
    ServerPlayerPositionMessage,
    ServerRemovePlayerMessage,
    ServerPreviewDescriptorMessage,
    ServerWorldRevisionMessage,
    ServerChunkPreviewMessage>;

[[nodiscard]]
constexpr int32_t normalizePreviewCoordinate(int64_t const value, int32_t const extent) noexcept
{
    if (extent <= 0) {
        return 0;
    }
    int64_t result = value % static_cast<int64_t>(extent);
    if (result < 0) {
        result += extent;
    }
    return static_cast<int32_t>(result);
}

[[nodiscard]]
constexpr PreviewChunkKey normalizePreviewChunkKey(PreviewChunkKey const key) noexcept
{
    return {
        .x = normalizePreviewCoordinate(key.x, 4'096),
        .y = normalizePreviewCoordinate(key.y, 4'096),
        .z = key.z,
    };
}

[[nodiscard]]
constexpr bool isNewerSequence(uint32_t const candidate, uint32_t const reference) noexcept
{
    return candidate != reference && candidate - reference < (uint32_t{ 1 } << 31U);
}

std::vector<uint8_t> encodeMessage(Message message);
std::optional<Message> decodeMessage(std::span<uint8_t const> data);

} // namespace shared
