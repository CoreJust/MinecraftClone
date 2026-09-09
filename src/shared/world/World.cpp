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
        } while (!canPlayerBeAt(x, y, id));
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

bool World::movePlayer(PlayerId const id, Direction const direction) {
    Player* p = nullptr;
    for (Player& player : m_players) {
        if (player.id == id) {
            p = &player;
        }
    }
    if (!p) {
        return false;
    }

    int const target_x = static_cast<int>(p->x) + static_cast<int8_t>(direction.x);
    int const target_y = static_cast<int>(p->y) + static_cast<int8_t>(direction.y);
    if (target_x < 0 || target_x >= WIDTH || target_y < 0 || target_y >= HEIGHT) {
        return false;
    }
    if (!canPlayerBeAt(static_cast<uint8_t>(target_x), static_cast<uint8_t>(target_y), id)) {
        return false;
    }

    p->x = static_cast<uint8_t>(target_x);
    p->y = static_cast<uint8_t>(target_y);
    return true;
}

void World::setPlayerPosition(PlayerId const id, uint8_t const x, uint8_t y) {
    for (Player& p : m_players) {
        if (p.id == id) {
            p.x = x;
            p.y = y;
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

bool World::canPlayerBeAt(uint8_t const x, uint8_t const y, PlayerId const id) const {
    if (x >= WIDTH || y >= HEIGHT) {
        return false;
    }
    // Inefficient, but simple
    for (Player const& player : m_players) {
        if (player.id != id) {
            int const min_x = std::max(0, static_cast<int>(player.x) - 1);
            int const max_x = std::min(static_cast<int>(WIDTH) - 1, static_cast<int>(player.x) + 1);
            int const min_y = std::max(0, static_cast<int>(player.y) - 1);
            int const max_y = std::min(static_cast<int>(HEIGHT) - 1, static_cast<int>(player.y) + 1);
            for (int p_x = min_x; p_x <= max_x; ++p_x) {
                for (int p_y = min_y; p_y <= max_y; ++p_y) {
                    if (p_x == x && p_y == y) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

} // namespace shared
