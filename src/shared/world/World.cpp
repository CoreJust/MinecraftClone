#include <shared/world/World.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <random>

namespace shared {

bool World::playerExists(char const ch) const noexcept {
    for (Player const& player : m_players) {
        if (player.ch == ch) {
            return true;
        }
    }
    return false;
}

void World::spawnPlayer(PlayerId const id, char const ch, std::optional<std::pair<uint8_t, uint8_t>> const at) {
    ASSERT(!playerExists(ch));
    uint8_t x;
    uint8_t y;
    if (at) {
        x = at->first;
        y = at->second;
    } else {
        static std::default_random_engine generator{ std::random_device{}() };
        std::uniform_int_distribution<uint16_t> x_distribution{ 0, WIDTH - 1 };
        std::uniform_int_distribution<uint16_t> y_distribution{ 0, HEIGHT - 1 };
        do {
            x = static_cast<uint8_t>(x_distribution(generator));
            y = static_cast<uint8_t>(y_distribution(generator));
        } while (!canPlayerBeAt(
            static_cast<uint32_t>(x) * SUBCELLS_PER_CELL,
            static_cast<uint32_t>(y) * SUBCELLS_PER_CELL,
            id
        ));
    }

    m_players.emplace_back(Player{
        .id = id,
        .x = x,
        .y = y,
        .ch = ch,
    });
}

void World::despawnPlayer(PlayerId const id) {
    for (size_t i{ 0 }; i < m_players.size(); ++i) {
        if (m_players[i].id == id) {
            std::swap(m_players[i], m_players.back());
            m_players.pop_back();
            break;
        }
    }
}

bool World::movePlayer(
    PlayerId const id,
    Direction const direction,
    std::chrono::milliseconds const elapsed
) {
    Player* p = nullptr;
    for (Player& player : m_players) {
        if (player.id == id) {
            p = &player;
        }
    }
    if (!p) {
        return false;
    }
    if (elapsed < std::chrono::milliseconds::zero() || elapsed > TICK) {
        return false;
    }

    int const direction_x = static_cast<int8_t>(direction.x);
    int const direction_y = static_cast<int8_t>(direction.y);
    if (direction_x == 0 && direction_y == 0) {
        return true;
    }

    uint32_t const squared_direction_length = static_cast<uint32_t>(
        direction_x * direction_x + direction_y * direction_y
    );
    uint32_t direction_length = 0;
    while (direction_length * direction_length < squared_direction_length) {
        ++direction_length;
    }
    uint32_t const divisor = std::max<uint32_t>(127U, direction_length);
    uint32_t const base_step = static_cast<uint32_t>(elapsed.count())
        * MOVEMENT_SUBCELLS_PER_TICK / static_cast<uint32_t>(TICK.count());
    int const delta_x = static_cast<int>(base_step * static_cast<uint32_t>(std::abs(direction_x)) / divisor)
        * (direction_x < 0 ? -1 : 1);
    int const delta_y = static_cast<int>(base_step * static_cast<uint32_t>(std::abs(direction_y)) / divisor)
        * (direction_y < 0 ? -1 : 1);
    int const position_x = static_cast<int>(p->x) * SUBCELLS_PER_CELL + p->x_subcell + delta_x;
    int const position_y = static_cast<int>(p->y) * SUBCELLS_PER_CELL + p->y_subcell + delta_y;
    int const target_x = position_x / SUBCELLS_PER_CELL;
    int const target_y = position_y / SUBCELLS_PER_CELL;
    if (position_x < 0 || position_x >= static_cast<int>(WIDTH) * SUBCELLS_PER_CELL
        || position_y < 0 || position_y >= static_cast<int>(HEIGHT) * SUBCELLS_PER_CELL) {
        return false;
    }
    if (!canPlayerBeAt(static_cast<uint32_t>(position_x), static_cast<uint32_t>(position_y), id)) {
        return false;
    }

    p->x = static_cast<uint8_t>(target_x);
    p->y = static_cast<uint8_t>(target_y);
    p->x_subcell = static_cast<uint16_t>(position_x % SUBCELLS_PER_CELL);
    p->y_subcell = static_cast<uint16_t>(position_y % SUBCELLS_PER_CELL);
    return true;
}

void World::setPlayerPosition(
    PlayerId const id,
    uint8_t const x,
    uint8_t const y,
    uint16_t const x_subcell,
    uint16_t const y_subcell
) {
    for (Player& p : m_players) {
        if (p.id == id) {
            p.x = x;
            p.y = y;
            p.x_subcell = x_subcell;
            p.y_subcell = y_subcell;
            break;
        }
    }
}

std::optional<Player> World::player(PlayerId const id) const noexcept {
    for (Player const& player : m_players) {
        if (player.id == id) {
            return player;
        }
    }
    return std::nullopt;
}

std::optional<Player> World::playerByCharacter(char const ch) const noexcept {
    for (Player const& player : m_players) {
        if (player.ch == ch) {
            return player;
        }
    }
    return std::nullopt;
}

bool World::canPlayerBeAt(uint32_t const x, uint32_t const y, PlayerId const id) const {
    if (x >= static_cast<uint32_t>(WIDTH) * SUBCELLS_PER_CELL
        || y >= static_cast<uint32_t>(HEIGHT) * SUBCELLS_PER_CELL) {
        return false;
    }
    constexpr uint32_t PLAYER_BOX_SIZE = 2U * SUBCELLS_PER_CELL;
    for (Player const& player : m_players) {
        if (player.id != id) {
            uint32_t const player_x = static_cast<uint32_t>(player.x) * SUBCELLS_PER_CELL + player.x_subcell;
            uint32_t const player_y = static_cast<uint32_t>(player.y) * SUBCELLS_PER_CELL + player.y_subcell;
            bool const overlaps_x = x < player_x + PLAYER_BOX_SIZE && player_x < x + PLAYER_BOX_SIZE;
            bool const overlaps_y = y < player_y + PLAYER_BOX_SIZE && player_y < y + PLAYER_BOX_SIZE;
            if (overlaps_x && overlaps_y) {
                return false;
            }
        }
    }
    return true;
}

} // namespace shared
