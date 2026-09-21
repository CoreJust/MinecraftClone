#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace shared {

constexpr std::chrono::milliseconds TICK { 100 };
constexpr uint16_t SUBCELLS_PER_CELL = 10'000;
constexpr uint16_t MOVEMENT_SUBCELLS_PER_TICK = 5'600;

using PlayerId = uint32_t;
using PlayerPaletteIndex = uint8_t;

constexpr PlayerPaletteIndex PLAYER_PALETTE_COUNT = 16U;

[[nodiscard]]
constexpr bool isValidPlayerPaletteIndex(PlayerPaletteIndex const index) noexcept
{
    return index < PLAYER_PALETTE_COUNT;
}

// This is intentionally a stable pseudo-random assignment: the server owns
// the result and repeats it when the same character identity reconnects.
[[nodiscard]]
constexpr PlayerPaletteIndex defaultPlayerPaletteIndex(char const character) noexcept
{
    uint32_t value = static_cast<uint8_t>(character);
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    return static_cast<PlayerPaletteIndex>(value & (PLAYER_PALETTE_COUNT - 1U));
}

enum class WorldMode : uint8_t {
    Flat,
    Flight,
};

enum class MovementCapability : uint8_t {
    Flight = 1U,
    CollisionBypass = 2U,
};

struct MovementCapabilities final {
    uint8_t bits = 0U;

    [[nodiscard]]
    constexpr bool allows(MovementCapability const capability) const noexcept
    {
        return (bits & static_cast<uint8_t>(capability)) != 0U;
    }

    constexpr bool operator==(MovementCapabilities const&) const noexcept = default;
};

[[nodiscard]]
constexpr bool isValidMovementCapabilities(MovementCapabilities const capabilities) noexcept
{
    constexpr uint8_t VALID_BITS = static_cast<uint8_t>(MovementCapability::Flight)
        | static_cast<uint8_t>(MovementCapability::CollisionBypass);
    return (capabilities.bits & ~VALID_BITS) == 0U
        && (!capabilities.allows(MovementCapability::CollisionBypass)
            || capabilities.allows(MovementCapability::Flight));
}

struct WorldConfiguration final {
    static constexpr uint32_t ALGORITHM_VERSION = 1;
    static constexpr uint64_t SEED = 42;
    static constexpr uint8_t CHUNK_DIMENSION = 16;

    uint32_t algorithm_version = ALGORITHM_VERSION;
    uint64_t seed = SEED;
    uint8_t chunk_width = CHUNK_DIMENSION;
    uint8_t chunk_height = CHUNK_DIMENSION;
    uint8_t chunk_depth = CHUNK_DIMENSION;
    uint64_t chunk_content_digest = 0;

    constexpr bool operator==(WorldConfiguration const&) const noexcept = default;
};

[[nodiscard]]
constexpr bool isValidWorldConfiguration(WorldConfiguration const& configuration) noexcept
{
    return configuration.algorithm_version != 0
        && configuration.chunk_width != 0
        && configuration.chunk_height != 0
        && configuration.chunk_depth != 0
        && configuration.chunk_content_digest != 0;
}

struct PlayerPosition final {
    int32_t x;
    int32_t y;
    int32_t z;
    uint16_t x_subcell = 0;
    uint16_t y_subcell = 0;
    uint16_t z_subcell = 0;
};

struct Player final {
    PlayerId id;
    int32_t x;
    int32_t y;
    int32_t z = 0;
    uint16_t x_subcell = 0;
    uint16_t y_subcell = 0;
    uint16_t z_subcell = 0;
    char ch;
    PlayerPaletteIndex palette_index = 0U;
    MovementCapabilities movement_capabilities{};
    int32_t vertical_velocity_subcells = 0;
};

struct Direction final {
    uint8_t x;
    uint8_t y;
    uint8_t z = 0;
    bool accelerated = false;
    uint16_t speedup = 5U;
    bool cycle_movement_capabilities = false;
    int8_t view_x = 0;
    int8_t view_y = 127;
};

inline constexpr std::array<uint16_t, 9> FLIGHT_SPEEDUP_PROFILES{2U, 3U, 5U, 8U, 15U, 30U, 80U, 200U, 500U};

[[nodiscard]]
constexpr bool isFlightSpeedupProfile(uint16_t const speedup) noexcept
{
    for (uint16_t const profile : FLIGHT_SPEEDUP_PROFILES) {
        if (profile == speedup) {
            return true;
        }
    }
    return false;
}

[[nodiscard]]
constexpr double playerPositionX(Player const& player) noexcept {
    return static_cast<double>(player.x)
        + static_cast<double>(player.x_subcell) / static_cast<double>(SUBCELLS_PER_CELL);
}

[[nodiscard]]
constexpr double playerPositionY(Player const& player) noexcept {
    return static_cast<double>(player.y)
        + static_cast<double>(player.y_subcell) / static_cast<double>(SUBCELLS_PER_CELL);
}

[[nodiscard]]
constexpr double playerPositionZ(Player const& player) noexcept {
    return static_cast<double>(player.z)
        + static_cast<double>(player.z_subcell) / static_cast<double>(SUBCELLS_PER_CELL);
}

class World final {
public:
    static constexpr uint8_t WIDTH = 32;
    static constexpr uint8_t HEIGHT = 32;
    static constexpr uint8_t PLAYER_FOOTPRINT_CELLS = 2;
    static constexpr uint32_t PLAYER_WIDTH_SUBCELLS = 6'250U;
    static constexpr uint32_t PLAYER_HEIGHT_SUBCELLS = 18'125U;
    static constexpr int32_t PLAYER_JUMP_IMPULSE_SUBCELLS = 11'200;
    static constexpr uint8_t MAX_PLAYER_ORIGIN_CELL = WIDTH - PLAYER_FOOTPRINT_CELLS;
    static constexpr uint32_t MAX_PLAYER_ORIGIN_SUBCELL = static_cast<uint32_t>(MAX_PLAYER_ORIGIN_CELL)
        * SUBCELLS_PER_CELL;
    static constexpr int32_t FLIGHT_MIN_CELL = 0;
    static constexpr int32_t FLIGHT_MAX_CELL = 65'535;
    static constexpr int32_t FLIGHT_MAX_Z = 1'023;
    static constexpr PlayerPosition FLIGHT_SPAWN{ .x = 32'896, .y = 32'768, .z = 410 };

    explicit World(
        WorldMode mode = WorldMode::Flat,
        WorldConfiguration configuration = canonicalConfiguration()
    ) noexcept;

    [[nodiscard]]
    static WorldConfiguration canonicalConfiguration();
    [[nodiscard]]
    static bool isFlightPositionInBounds(PlayerPosition position) noexcept;

    [[nodiscard]]
    constexpr WorldMode mode() const noexcept { return m_mode; }
    [[nodiscard]]
    constexpr WorldConfiguration const& configuration() const noexcept { return m_configuration; }
    [[nodiscard]]
    bool playerExists(char ch) const noexcept;
    void spawnPlayer(
        PlayerId id,
        char ch,
        std::optional<std::pair<uint8_t, uint8_t>> const& at = std::nullopt,
        PlayerPaletteIndex palette_index = 0U
    );
    void spawnPlayer(PlayerId id, char ch, PlayerPosition at, PlayerPaletteIndex palette_index = 0U);
    void despawnPlayer(PlayerId id);

    [[nodiscard]]
    bool movePlayer(
        PlayerId id,
        Direction direction,
        std::chrono::milliseconds elapsed = TICK
    );

    [[nodiscard]]
    bool setPlayerPosition(
        PlayerId id,
        uint8_t x,
        uint8_t y,
        uint16_t x_subcell = 0,
        uint16_t y_subcell = 0
    );
    [[nodiscard]]
    bool setPlayerPosition(PlayerId id, PlayerPosition position);
    bool setPlayerPaletteIndex(PlayerId id, PlayerPaletteIndex palette_index) noexcept;
    [[nodiscard]]
    bool setPlayerMovementCapabilities(PlayerId id, MovementCapabilities capabilities) noexcept;

    [[nodiscard]]
    std::optional<Player> player(PlayerId id) const noexcept;
    [[nodiscard]]
    std::optional<Player> playerByCharacter(char ch) const noexcept;
    [[nodiscard]]
    constexpr std::vector<Player> const& players() const noexcept { return m_players; }
private:
    [[nodiscard]]
    bool canPlayerBeAt(uint32_t x, uint32_t y, int64_t z, PlayerId id) const;
    [[nodiscard]]
    bool canFlightPlayerBeAt(
        Player const& player,
        int64_t x_subcells,
        int64_t y_subcells,
        int64_t z_subcells
    ) const;
    [[nodiscard]]
    static PlayerPosition positionFromSubcells(int32_t x, int32_t y, int32_t z) noexcept;
    [[nodiscard]]
    bool moveFlightPlayer(Player& player, Direction direction, std::chrono::milliseconds elapsed) const;
private:
    WorldMode m_mode;
    WorldConfiguration m_configuration;
    std::vector<Player> m_players;
};

} // namespace shared
