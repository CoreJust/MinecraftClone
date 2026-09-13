#pragma once

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

enum class WorldMode : uint8_t {
    Flat,
    Flight,
};

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
};

struct Direction final {
    uint8_t x;
    uint8_t y;
    uint8_t z = 0;
};

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
    static constexpr uint8_t MAX_PLAYER_ORIGIN_CELL = WIDTH - PLAYER_FOOTPRINT_CELLS;
    static constexpr uint32_t MAX_PLAYER_ORIGIN_SUBCELL = static_cast<uint32_t>(MAX_PLAYER_ORIGIN_CELL)
        * SUBCELLS_PER_CELL;
    static constexpr int32_t FLIGHT_MIN_CELL = -64;
    static constexpr int32_t FLIGHT_MAX_CELL = 64;
    static constexpr PlayerPosition FLIGHT_SPAWN{ .x = 8, .y = 8, .z = 12 };

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
        std::optional<std::pair<uint8_t, uint8_t>> const& at = std::nullopt
    );
    void spawnPlayer(PlayerId id, char ch, PlayerPosition at);
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

    [[nodiscard]]
    std::optional<Player> player(PlayerId id) const noexcept;
    [[nodiscard]]
    std::optional<Player> playerByCharacter(char ch) const noexcept;
    [[nodiscard]]
    constexpr std::vector<Player> const& players() const noexcept { return m_players; }
private:
    [[nodiscard]]
    bool canPlayerBeAt(uint32_t x, uint32_t y, PlayerId id) const;
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
