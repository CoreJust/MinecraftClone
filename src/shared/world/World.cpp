#include <shared/world/World.hpp>

#include <core/Assert.hpp>

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
        uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        do {
            x = now % (WIDTH - 1);
            now = now * (now + 1);
            y = now % (HEIGHT - 1);
        } while (!canPlayerBeAt(x, y, ch));
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

    auto const [off_x, off_y] = direction;
    if (!canPlayerBeAt(p->x + off_x, p->y + off_y, id)) {
        return false;
    }

    p->x += off_x;
    p->y += off_y;
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
    if (x >= WIDTH - 1 || y >= HEIGHT - 1) {
        return false;
    }
    // Inefficient, but simple
    for (Player const& player : m_players) {
        if (player.id != id) {
            for (uint8_t p_x = player.x - 1; p_x <= player.x + 1; ++p_x) {
                for (uint8_t p_y = player.y - 1; p_y <= player.y + 1; ++p_y) {
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
