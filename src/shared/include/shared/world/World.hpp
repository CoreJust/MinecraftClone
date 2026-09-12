#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace shared {

constexpr std::chrono::milliseconds TICK { 100 };
constexpr uint16_t SUBCELLS_PER_CELL = 10'000;
constexpr uint16_t MOVEMENT_SUBCELLS_PER_TICK = 1'000;

using PlayerId = uint32_t;

struct Player final {
    PlayerId id;
    uint8_t x;
    uint8_t y;
    uint16_t x_subcell = 0;
    uint16_t y_subcell = 0;
    char ch;
};

struct Direction final {
    uint8_t x;
    uint8_t y;
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

class World final {
public:
    static constexpr uint8_t WIDTH = 32;
    static constexpr uint8_t HEIGHT = 32;

    [[nodiscard]]
    bool playerExists(char const ch) const noexcept;
    void spawnPlayer(PlayerId const id, char const ch, std::optional<std::pair<uint8_t, uint8_t>> const at = std::nullopt);
    void despawnPlayer(PlayerId const id);

    // Return true if move is possible
    [[nodiscard]]
    bool movePlayer(
        PlayerId id,
        Direction direction,
        std::chrono::milliseconds elapsed = TICK
    );

    void setPlayerPosition(
        PlayerId id,
        uint8_t x,
        uint8_t y,
        uint16_t x_subcell = 0,
        uint16_t y_subcell = 0
    );

    [[nodiscard]]
    std::optional<Player> player(PlayerId const id) const noexcept;
    [[nodiscard]]
    std::optional<Player> playerByCharacter(char const ch) const noexcept;
    [[nodiscard]]
    constexpr std::vector<Player> const& players() const noexcept { return m_players; }
private:
    [[nodiscard]]
    bool canPlayerBeAt(uint32_t x, uint32_t y, PlayerId id) const;
private:
    std::vector<Player> m_players;
};

} // namespace shared
