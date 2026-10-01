#pragma once

#include <shared/world/World.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace shared {

constexpr uint8_t PROTOCOL_MAGIC = 0x4DU;
constexpr uint8_t PROTOCOL_VERSION = 12U;
constexpr uint8_t GAME_CHANNEL = 0U;
constexpr uint8_t HEIGHT_TILE_CHANNEL = 1U;
constexpr uint32_t HEIGHT_TILE_SIDE_LENGTH = 16U;
constexpr uint32_t HEIGHT_TILE_SAMPLE_COUNT = HEIGHT_TILE_SIDE_LENGTH * HEIGHT_TILE_SIDE_LENGTH;
constexpr uint32_t HEIGHT_TILE_PAYLOAD_BYTES = HEIGHT_TILE_SAMPLE_COUNT * sizeof(uint16_t);
constexpr uint32_t HEIGHT_TILE_INTEREST_RADIUS = 256U;
constexpr uint32_t MIN_HEIGHT_TILE_INTEREST_RADIUS = 1U;
constexpr uint32_t MAX_HEIGHT_TILE_INTEREST_RADIUS = 256U;
constexpr uint32_t HEIGHT_TILE_INTEREST_WIDTH = HEIGHT_TILE_INTEREST_RADIUS * 2U + 1U;

[[nodiscard]]
constexpr uint32_t heightTileInterestCount(uint32_t const radius) noexcept
{
    uint32_t const signed_radius = radius;
    uint32_t count = 0U;
    for (int32_t y = -static_cast<int32_t>(signed_radius);
         y <= static_cast<int32_t>(signed_radius);
         ++y) {
        uint32_t const y_squared = static_cast<uint32_t>(y * y);
        uint32_t low = 0U;
        uint32_t high = signed_radius;
        while (low < high) {
            uint32_t const middle = low + (high - low + 1U) / 2U;
            if (middle * middle <= radius * radius - y_squared) {
                low = middle;
            } else {
                high = middle - 1U;
            }
        }
        count += low * 2U + 1U;
    }
    return count;
}

constexpr uint32_t HEIGHT_TILE_INTEREST_COUNT = heightTileInterestCount(HEIGHT_TILE_INTEREST_RADIUS);

[[nodiscard]]
constexpr bool isValidHeightTileInterestRadius(uint32_t const radius) noexcept
{
    return radius >= MIN_HEIGHT_TILE_INTEREST_RADIUS && radius <= MAX_HEIGHT_TILE_INTEREST_RADIUS;
}

[[nodiscard]]
constexpr std::optional<uint32_t> heightTileInterestRadiusForCount(uint32_t const count) noexcept
{
    uint32_t low = MIN_HEIGHT_TILE_INTEREST_RADIUS;
    uint32_t high = MAX_HEIGHT_TILE_INTEREST_RADIUS;
    while (low <= high) {
        uint32_t const radius = low + (high - low) / 2U;
        uint32_t const disk_count = heightTileInterestCount(radius);
        if (disk_count == count) {
            return radius;
        }
        if (disk_count < count) {
            low = radius + 1U;
        } else {
            high = radius - 1U;
        }
    }
    return std::nullopt;
}
constexpr uint8_t HEIGHT_TILE_BATCH_CAPACITY = 16U;
constexpr uint8_t HEIGHT_TILE_DELIVERY_WINDOW = 8U;
constexpr uint8_t HEIGHT_TILE_DELIVERY_BATCH_CAPACITY = HEIGHT_TILE_BATCH_CAPACITY;

struct HeightTileKey final {
    int32_t x;
    int32_t y;

    constexpr bool operator==(HeightTileKey const&) const noexcept = default;
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

// Credits are transport admission permits, not generation requests. A client
// returns one only after it has coalesced the matching stream operation into
// its bounded terrain handoff.
struct ClientHeightTileCreditMessage final {
    uint64_t world_revision = 0U;
    uint64_t delivery_token = 0U;
    uint8_t credits = 0U;
};

struct ServerPlayerPositionMessage final {
    char ch;
    PlayerPaletteIndex palette_index = 0U;
    MovementCapabilities movement_capabilities{};
    int32_t x;
    int32_t y;
    int32_t z = 0;
    uint16_t x_subcell;
    uint16_t y_subcell;
    uint16_t z_subcell = 0;
    int32_t vertical_velocity_subcells = 0;
    uint32_t acknowledged_input_sequence = 0;
    uint32_t state_revision = 0;
};

struct ServerRemovePlayerMessage final {
    char ch;
};

struct ServerHeightTileDescriptorMessage final {
    WorldConfiguration configuration = World::canonicalConfiguration();
    uint64_t world_revision = 1;
    uint32_t max_height_tiles = HEIGHT_TILE_INTEREST_COUNT;
    uint32_t max_height_tile_bytes = HEIGHT_TILE_PAYLOAD_BYTES;
};

struct ServerWorldRevisionMessage final {
    uint64_t world_revision = 1;
};

struct ServerHeightTileMessage final {
    HeightTileKey key{};
    uint64_t revision = 1;
    uint64_t token = 0;
    std::array<uint16_t, HEIGHT_TILE_SAMPLE_COUNT> heights{};
};

struct ServerRemoveHeightTileMessage final {
    HeightTileKey key{};
    uint64_t revision = 1;
    uint64_t token = 0;
};

struct ServerHeightTileBatchMessage final {
    uint64_t delivery_token = 0U;
    std::vector<ServerHeightTileMessage> tiles;
    std::vector<ServerRemoveHeightTileMessage> removals;
};

using Message = std::variant<
    JoinRequestMessage,
    JoinResponseMessage,
    ClientInputMessage,
    ClientHeightTileCreditMessage,
    ServerPlayerPositionMessage,
    ServerRemovePlayerMessage,
    ServerHeightTileDescriptorMessage,
    ServerWorldRevisionMessage,
    ServerHeightTileMessage,
    ServerHeightTileBatchMessage,
    ServerRemoveHeightTileMessage>;

[[nodiscard]]
constexpr int32_t normalizeHeightTileCoordinate(int64_t const value, int32_t const extent) noexcept
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
constexpr HeightTileKey normalizeHeightTileKey(HeightTileKey const key) noexcept
{
    return {
        .x = normalizeHeightTileCoordinate(key.x, 4'096),
        .y = normalizeHeightTileCoordinate(key.y, 4'096),
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
