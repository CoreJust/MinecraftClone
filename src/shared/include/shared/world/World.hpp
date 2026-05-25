#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace shared {

constexpr std::chrono::milliseconds TICK { 100 };

using PlayerId = uint32_t;

struct Player final {
    PlayerId id;
    uint8_t x;
    uint8_t y;
    char ch;
};

struct Direction final {
    uint8_t x;
    uint8_t y;
};

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
    bool movePlayer(PlayerId const id, Direction const direction);

    void setPlayerPosition(PlayerId const id, uint8_t const x, uint8_t y);

    [[nodiscard]]
    std::optional<Player> player(PlayerId const id) const noexcept;
    [[nodiscard]]
    std::optional<Player> playerByCharacter(char const ch) const noexcept;
    [[nodiscard]]
    constexpr std::vector<Player> const& players() const noexcept { return m_players; }
private:
    [[nodiscard]]
    bool canPlayerBeAt(uint8_t const x, uint8_t const y, PlayerId const id) const;
private:
    std::vector<Player> m_players;
};

} // namespace shared
