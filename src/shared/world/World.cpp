#include <shared/world/World.hpp>

#include <shared/world/CanonicalWorld.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <cstdlib>
#include <random>

namespace shared {

World::World(WorldMode const mode, WorldConfiguration const configuration) noexcept
    : m_mode(mode)
    , m_configuration(configuration)
{
}

WorldConfiguration World::canonicalConfiguration()
{
    return canonicalWorld().configuration();
}

bool World::playerExists(char const ch) const noexcept
{
    for (Player const& player : m_players) {
        if (player.ch == ch) {
            return true;
        }
    }
    return false;
}

void World::spawnPlayer(
    PlayerId const id,
    char const ch,
    std::optional<std::pair<uint8_t, uint8_t>> const& at
) {
    ASSERT(!playerExists(ch));
    if (m_mode == WorldMode::Flight) {
        spawnPlayer(id, ch, FLIGHT_SPAWN);
        return;
    }

    uint8_t x;
    uint8_t y;
    if (at) {
        x = at->first;
        y = at->second;
    } else {
        static std::default_random_engine generator{ std::random_device{}() };
        std::vector<std::pair<uint8_t, uint8_t>> spawn_locations;
        spawn_locations.reserve(static_cast<uint32_t>(MAX_PLAYER_ORIGIN_CELL + 1U)
            * static_cast<uint32_t>(MAX_PLAYER_ORIGIN_CELL + 1U));
        for (uint8_t candidate_x{ 0 }; candidate_x <= MAX_PLAYER_ORIGIN_CELL; ++candidate_x) {
            for (uint8_t candidate_y{ 0 }; candidate_y <= MAX_PLAYER_ORIGIN_CELL; ++candidate_y) {
                if (canPlayerBeAt(
                    static_cast<uint32_t>(candidate_x) * SUBCELLS_PER_CELL,
                    static_cast<uint32_t>(candidate_y) * SUBCELLS_PER_CELL,
                    id
                )) {
                    spawn_locations.emplace_back(candidate_x, candidate_y);
                }
            }
        }
        ASSERT(!spawn_locations.empty());
        std::uniform_int_distribution<uint32_t> location_distribution{
            0U,
            static_cast<uint32_t>(spawn_locations.size() - 1U),
        };
        auto const& location = spawn_locations[location_distribution(generator)];
        x = location.first;
        y = location.second;
    }

    m_players.emplace_back(Player{
        .id = id,
        .x = x,
        .y = y,
        .ch = ch,
    });
}

void World::spawnPlayer(PlayerId const id, char const ch, PlayerPosition const at)
{
    ASSERT(!playerExists(ch));
    if (m_mode == WorldMode::Flight) {
        ASSERT(isFlightPositionInBounds(at));
    } else {
        ASSERT(at.x >= 0 && at.x <= MAX_PLAYER_ORIGIN_CELL);
        ASSERT(at.y >= 0 && at.y <= MAX_PLAYER_ORIGIN_CELL);
        ASSERT(at.z == 0);
        ASSERT(at.x_subcell < SUBCELLS_PER_CELL);
        ASSERT(at.y_subcell < SUBCELLS_PER_CELL);
        ASSERT(at.z_subcell == 0);
    }
    m_players.emplace_back(Player{
        .id = id,
        .x = at.x,
        .y = at.y,
        .z = at.z,
        .x_subcell = at.x_subcell,
        .y_subcell = at.y_subcell,
        .z_subcell = at.z_subcell,
        .ch = ch,
    });
}

void World::despawnPlayer(PlayerId const id)
{
    for (auto player = m_players.begin(); player != m_players.end(); ++player) {
        if (player->id == id) {
            std::swap(*player, m_players.back());
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
    Player* player = nullptr;
    for (Player& candidate : m_players) {
        if (candidate.id == id) {
            player = &candidate;
            break;
        }
    }
    if (player == nullptr || elapsed < std::chrono::milliseconds::zero() || elapsed > TICK) {
        return false;
    }
    if (m_mode == WorldMode::Flight) {
        return moveFlightPlayer(*player, direction, elapsed);
    }

    int32_t const direction_x = static_cast<int8_t>(direction.x);
    int32_t const direction_y = static_cast<int8_t>(direction.y);
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
    int32_t const delta_x = static_cast<int32_t>(base_step * static_cast<uint32_t>(std::abs(direction_x)) / divisor)
        * (direction_x < 0 ? -1 : 1);
    int32_t const delta_y = static_cast<int32_t>(base_step * static_cast<uint32_t>(std::abs(direction_y)) / divisor)
        * (direction_y < 0 ? -1 : 1);
    int32_t position_x = player->x * SUBCELLS_PER_CELL + player->x_subcell;
    int32_t position_y = player->y * SUBCELLS_PER_CELL + player->y_subcell;
    bool applied_x = false;
    bool applied_y = false;
    int32_t const candidate_x = position_x + delta_x;
    if (delta_x != 0 && candidate_x >= 0 && candidate_x <= static_cast<int32_t>(MAX_PLAYER_ORIGIN_SUBCELL)
        && canPlayerBeAt(static_cast<uint32_t>(candidate_x), static_cast<uint32_t>(position_y), id)) {
        position_x = candidate_x;
        applied_x = true;
    }
    int32_t const candidate_y = position_y + delta_y;
    if (delta_y != 0 && candidate_y >= 0 && candidate_y <= static_cast<int32_t>(MAX_PLAYER_ORIGIN_SUBCELL)
        && canPlayerBeAt(static_cast<uint32_t>(position_x), static_cast<uint32_t>(candidate_y), id)) {
        position_y = candidate_y;
        applied_y = true;
    }
    if (!applied_x && !applied_y) {
        return false;
    }

    player->x = position_x / SUBCELLS_PER_CELL;
    player->y = position_y / SUBCELLS_PER_CELL;
    player->x_subcell = static_cast<uint16_t>(position_x % SUBCELLS_PER_CELL);
    player->y_subcell = static_cast<uint16_t>(position_y % SUBCELLS_PER_CELL);
    return true;
}

bool World::setPlayerPosition(
    PlayerId const id,
    uint8_t const x,
    uint8_t const y,
    uint16_t const x_subcell,
    uint16_t const y_subcell
) {
    return setPlayerPosition(id, PlayerPosition{
        .x = x,
        .y = y,
        .z = 0,
        .x_subcell = x_subcell,
        .y_subcell = y_subcell,
    });
}

bool World::setPlayerPosition(PlayerId const id, PlayerPosition const position)
{
    if ((m_mode == WorldMode::Flight && !isFlightPositionInBounds(position))
        || (m_mode == WorldMode::Flat
            && (position.x < 0 || position.x > MAX_PLAYER_ORIGIN_CELL
                || position.y < 0 || position.y > MAX_PLAYER_ORIGIN_CELL || position.z != 0
                || position.x_subcell >= SUBCELLS_PER_CELL || position.y_subcell >= SUBCELLS_PER_CELL
                || position.z_subcell != 0))) {
        return false;
    }
    for (Player& player : m_players) {
        if (player.id == id) {
            player.x = position.x;
            player.y = position.y;
            player.z = position.z;
            player.x_subcell = position.x_subcell;
            player.y_subcell = position.y_subcell;
            player.z_subcell = position.z_subcell;
            return true;
        }
    }
    return false;
}

std::optional<Player> World::player(PlayerId const id) const noexcept
{
    for (Player const& player : m_players) {
        if (player.id == id) {
            return player;
        }
    }
    return std::nullopt;
}

std::optional<Player> World::playerByCharacter(char const ch) const noexcept
{
    for (Player const& player : m_players) {
        if (player.ch == ch) {
            return player;
        }
    }
    return std::nullopt;
}

bool World::canPlayerBeAt(uint32_t const x, uint32_t const y, PlayerId const id) const
{
    if (x > MAX_PLAYER_ORIGIN_SUBCELL || y > MAX_PLAYER_ORIGIN_SUBCELL) {
        return false;
    }
    constexpr uint32_t PLAYER_BOX_SIZE = PLAYER_FOOTPRINT_CELLS * SUBCELLS_PER_CELL;
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

bool World::isFlightPositionInBounds(PlayerPosition const position) noexcept
{
    return position.x >= FLIGHT_MIN_CELL && position.x <= FLIGHT_MAX_CELL
        && position.y >= FLIGHT_MIN_CELL && position.y <= FLIGHT_MAX_CELL
        && position.z >= FLIGHT_MIN_CELL && position.z <= FLIGHT_MAX_CELL
        && position.x_subcell < SUBCELLS_PER_CELL && position.y_subcell < SUBCELLS_PER_CELL
        && position.z_subcell < SUBCELLS_PER_CELL;
}

PlayerPosition World::positionFromSubcells(int32_t const x, int32_t const y, int32_t const z) noexcept
{
    auto const split = [](int32_t const fixed_position, int32_t& cell, uint16_t& subcell) {
        int32_t remainder = fixed_position % SUBCELLS_PER_CELL;
        cell = fixed_position / SUBCELLS_PER_CELL;
        if (remainder < 0) {
            --cell;
            remainder += SUBCELLS_PER_CELL;
        }
        subcell = static_cast<uint16_t>(remainder);
    };

    PlayerPosition position{};
    split(x, position.x, position.x_subcell);
    split(y, position.y, position.y_subcell);
    split(z, position.z, position.z_subcell);
    return position;
}

bool World::moveFlightPlayer(
    Player& player,
    Direction const direction,
    std::chrono::milliseconds const elapsed
) const {
    int32_t const direction_x = static_cast<int8_t>(direction.x);
    int32_t const direction_y = static_cast<int8_t>(direction.y);
    int32_t const direction_z = static_cast<int8_t>(direction.z);
    if (direction_x == 0 && direction_y == 0 && direction_z == 0) {
        return true;
    }

    uint32_t const squared_direction_length = static_cast<uint32_t>(
        direction_x * direction_x + direction_y * direction_y + direction_z * direction_z
    );
    uint32_t direction_length = 0;
    while (direction_length * direction_length < squared_direction_length) {
        ++direction_length;
    }
    uint32_t const divisor = std::max<uint32_t>(127U, direction_length);
    uint32_t const base_step = static_cast<uint32_t>(elapsed.count())
        * MOVEMENT_SUBCELLS_PER_TICK / static_cast<uint32_t>(TICK.count());
    auto const movementDelta = [base_step, divisor](int32_t const component) {
        return static_cast<int32_t>(base_step * static_cast<uint32_t>(std::abs(component)) / divisor)
            * (component < 0 ? -1 : 1);
    };
    PlayerPosition const candidate = positionFromSubcells(
        player.x * SUBCELLS_PER_CELL + player.x_subcell + movementDelta(direction_x),
        player.y * SUBCELLS_PER_CELL + player.y_subcell + movementDelta(direction_y),
        player.z * SUBCELLS_PER_CELL + player.z_subcell + movementDelta(direction_z)
    );
    if (!isFlightPositionInBounds(candidate)) {
        return false;
    }
    player.x = candidate.x;
    player.y = candidate.y;
    player.z = candidate.z;
    player.x_subcell = candidate.x_subcell;
    player.y_subcell = candidate.y_subcell;
    player.z_subcell = candidate.z_subcell;
    return true;
}

} // namespace shared
