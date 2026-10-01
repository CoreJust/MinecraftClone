#include <shared/world/World.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/common/Assert.hpp>

#include <algorithm>
#include <cstdlib>
#include <random>

namespace shared {

namespace {

[[nodiscard]] TerrainGenerator const& collisionTerrain()
{
    static TerrainGenerator const terrain;
    return terrain;
}

[[nodiscard]] int64_t floorDivide(int64_t const value, int64_t const divisor) noexcept
{
    int64_t result = value / divisor;
    if (value < 0 && value % divisor != 0) {
        --result;
    }
    return result;
}

[[nodiscard]] int32_t wrapFlightCell(int64_t const cell) noexcept
{
    constexpr int64_t EXTENT = static_cast<int64_t>(World::FLIGHT_MAX_CELL) + 1;
    return static_cast<int32_t>((cell % EXTENT + EXTENT) % EXTENT);
}

[[nodiscard]] int64_t terrainSurfaceUnderPlayer(
    int64_t const x_subcells,
    int64_t const y_subcells
)
{
    constexpr int64_t WIDTH = static_cast<int64_t>(World::PLAYER_WIDTH_SUBCELLS);
    constexpr int64_t LAST_SUBCELL_OFFSET = WIDTH - 1;
    int64_t const first_x = floorDivide(x_subcells, SUBCELLS_PER_CELL);
    int64_t const last_x = floorDivide(x_subcells + LAST_SUBCELL_OFFSET, SUBCELLS_PER_CELL);
    int64_t const first_y = floorDivide(y_subcells, SUBCELLS_PER_CELL);
    int64_t const last_y = floorDivide(y_subcells + LAST_SUBCELL_OFFSET, SUBCELLS_PER_CELL);
    int64_t surface = 0;
    for (int64_t cell_x = first_x; cell_x <= last_x; ++cell_x) {
        for (int64_t cell_y = first_y; cell_y <= last_y; ++cell_y) {
            surface = std::max<int64_t>(
                surface,
                static_cast<int64_t>(collisionTerrain().heightAt(
                    wrapFlightCell(cell_x),
                    wrapFlightCell(cell_y)
                )) * SUBCELLS_PER_CELL
            );
        }
    }
    return surface;
}

[[nodiscard]] int64_t materializedTerrainSurfaceUnderPlayer(
    SparseWorld const& world,
    int64_t const x_subcells,
    int64_t const y_subcells,
    int64_t const upper_z_subcells
)
{
    if (upper_z_subcells < 0) {
        return 0;
    }

    constexpr int64_t WIDTH = static_cast<int64_t>(World::PLAYER_WIDTH_SUBCELLS);
    constexpr int64_t LAST_SUBCELL_OFFSET = WIDTH - 1;
    int64_t const first_x = floorDivide(x_subcells, SUBCELLS_PER_CELL);
    int64_t const last_x = floorDivide(x_subcells + LAST_SUBCELL_OFFSET, SUBCELLS_PER_CELL);
    int64_t const first_y = floorDivide(y_subcells, SUBCELLS_PER_CELL);
    int64_t const last_y = floorDivide(y_subcells + LAST_SUBCELL_OFFSET, SUBCELLS_PER_CELL);
    int64_t const upper_block = std::min<int64_t>(
        floorDivide(upper_z_subcells, SUBCELLS_PER_CELL),
        World::FLIGHT_MAX_Z
    );
    int64_t surface = 0;
    for (int64_t cell_x = first_x; cell_x <= last_x; ++cell_x) {
        int32_t const wrapped_x = wrapFlightCell(cell_x);
        for (int64_t cell_y = first_y; cell_y <= last_y; ++cell_y) {
            int32_t const wrapped_y = wrapFlightCell(cell_y);
            int64_t const chunk_x = wrapped_x / Chunk::SIDE_LENGTH;
            int64_t const chunk_y = wrapped_y / Chunk::SIDE_LENGTH;
            int64_t const upper_chunk_z = upper_block / Chunk::SIDE_LENGTH;
            bool found_column_support = false;
            for (int64_t chunk_z = upper_chunk_z; chunk_z >= 0; --chunk_z) {
                Chunk const* const chunk = world.residentChunk({
                    .x = static_cast<int32_t>(chunk_x),
                    .y = static_cast<int32_t>(chunk_y),
                    .z = static_cast<int32_t>(chunk_z),
                });
                if (chunk == nullptr) {
                    continue;
                }
                int64_t const chunk_bottom = chunk_z * Chunk::SIDE_LENGTH;
                int64_t const first_block_z = std::min<int64_t>(
                    upper_block,
                    chunk_bottom + Chunk::SIDE_LENGTH - 1
                );
                for (int64_t block_z = first_block_z; block_z >= chunk_bottom; --block_z) {
                    std::optional<Block> const block = chunk->blockAt({
                        .x = static_cast<uint8_t>(wrapped_x % Chunk::SIDE_LENGTH),
                        .y = static_cast<uint8_t>(wrapped_y % Chunk::SIDE_LENGTH),
                        .z = static_cast<uint8_t>(block_z % Chunk::SIDE_LENGTH),
                    });
                    if (block == Block::Stone) {
                        surface = std::max(surface, (block_z + 1) * SUBCELLS_PER_CELL);
                        found_column_support = true;
                        break;
                    }
                }
                if (found_column_support) {
                    break;
                }
            }
        }
    }
    return surface;
}

enum class UnknownCollisionPolicy : uint8_t {
    Block,
    Ignore,
};

[[nodiscard]] bool materializedBodyIsBlocked(
    SparseWorld const& world,
    int64_t const x_subcells,
    int64_t const y_subcells,
    int64_t const z_subcells,
    UnknownCollisionPolicy const unknown_policy
) noexcept
{
    int64_t const first_x = floorDivide(x_subcells, SUBCELLS_PER_CELL);
    int64_t const last_x = floorDivide(x_subcells + World::PLAYER_WIDTH_SUBCELLS - 1, SUBCELLS_PER_CELL);
    int64_t const first_y = floorDivide(y_subcells, SUBCELLS_PER_CELL);
    int64_t const last_y = floorDivide(y_subcells + World::PLAYER_WIDTH_SUBCELLS - 1, SUBCELLS_PER_CELL);
    int64_t const first_z = floorDivide(z_subcells, SUBCELLS_PER_CELL);
    int64_t const last_z = floorDivide(z_subcells + World::PLAYER_HEIGHT_SUBCELLS - 1, SUBCELLS_PER_CELL);
    for (int64_t cell_x = first_x; cell_x <= last_x; ++cell_x) {
        for (int64_t cell_y = first_y; cell_y <= last_y; ++cell_y) {
            for (int64_t cell_z = first_z; cell_z <= last_z; ++cell_z) {
                SparseBlockQuery const query = world.queryBlock({.x = cell_x, .y = cell_y, .z = cell_z});
                if (query.state != GenerationState::Materialized) {
                    if (unknown_policy == UnknownCollisionPolicy::Block) {
                        return true;
                    }
                } else if (query.block != Block::Air) {
                    return true;
                }
            }
        }
    }
    return false;
}

[[nodiscard]] bool isInsideTerrain(
    int64_t const x_subcells,
    int64_t const y_subcells,
    int64_t const z_subcells
)
{
    return z_subcells < terrainSurfaceUnderPlayer(x_subcells, y_subcells);
}

[[nodiscard]] bool linearIntervalsOverlap(
    int64_t const first,
    int64_t const second,
    int64_t const width
) noexcept
{
    return first < second + width && second < first + width;
}

[[nodiscard]] bool wrappedIntervalsOverlap(
    int64_t const first,
    int64_t const second,
    int64_t const width,
    int64_t const period
) noexcept
{
    int64_t const distance = first >= second ? first - second : second - first;
    return distance < width || period - distance < width;
}

} // namespace

World::World(WorldMode const mode, WorldConfiguration const configuration) noexcept
    : m_mode(mode)
    , m_configuration(configuration)
{
}

WorldConfiguration World::canonicalConfiguration()
{
    static WorldConfiguration const configuration = [] {
        Chunk const chunk = Chunk::makeStoneFixture({}, WorldConfiguration::SEED);
        return WorldConfiguration{
            .chunk_content_digest = chunk.contentIdentity().content_hash,
        };
    }();
    return configuration;
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
    std::optional<std::pair<uint8_t, uint8_t>> const& at,
    PlayerPaletteIndex const palette_index
) {
    ASSERT(!playerExists(ch));
    ASSERT(isValidPlayerPaletteIndex(palette_index));
    if (m_mode == WorldMode::Flight) {
        PlayerPosition spawn = FLIGHT_SPAWN;
        while (!canPlayerBeAt(
            static_cast<uint32_t>(spawn.x) * SUBCELLS_PER_CELL,
            static_cast<uint32_t>(spawn.y) * SUBCELLS_PER_CELL,
            static_cast<int64_t>(spawn.z) * SUBCELLS_PER_CELL,
            id
        )) {
            spawn.x = (spawn.x + PLAYER_FOOTPRINT_CELLS) % (FLIGHT_MAX_CELL + 1);
        }
        spawnPlayer(id, ch, spawn, palette_index);
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
                    0,
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
        .palette_index = palette_index,
        .movement_capabilities = m_mode == WorldMode::Flight
            ? MovementCapabilities{ .bits = 3U } : MovementCapabilities{},
    });
}

void World::spawnPlayer(
    PlayerId const id,
    char const ch,
    PlayerPosition const at,
    PlayerPaletteIndex const palette_index
)
{
    ASSERT(!playerExists(ch));
    ASSERT(isValidPlayerPaletteIndex(palette_index));
    PlayerPosition normalized = at;
    if (m_mode == WorldMode::Flight) {
        constexpr int32_t EXTENT = FLIGHT_MAX_CELL + 1;
        normalized.x = (normalized.x % EXTENT + EXTENT) % EXTENT;
        normalized.y = (normalized.y % EXTENT + EXTENT) % EXTENT;
        ASSERT(isFlightPositionInBounds(normalized));
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
        .x = normalized.x,
        .y = normalized.y,
        .z = normalized.z,
        .x_subcell = normalized.x_subcell,
        .y_subcell = normalized.y_subcell,
        .z_subcell = normalized.z_subcell,
        .ch = ch,
        .palette_index = palette_index,
        .movement_capabilities = m_mode == WorldMode::Flight
            ? MovementCapabilities{ .bits = 3U } : MovementCapabilities{},
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
    if (player->movement_capabilities.allows(MovementCapability::Flight)) {
        return moveFlightPlayer(*player, direction, elapsed);
    }

    int32_t const direction_x = static_cast<int8_t>(direction.x);
    int32_t const direction_y = static_cast<int8_t>(direction.y);
    int32_t const direction_z = static_cast<int8_t>(direction.z);
    if (m_mode == WorldMode::Flat && direction_x == 0 && direction_y == 0) {
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
    int32_t const maximum_horizontal_subcell = m_mode == WorldMode::Flight
        ? static_cast<int32_t>((static_cast<int64_t>(FLIGHT_MAX_CELL) + 1) * SUBCELLS_PER_CELL - 1)
        : static_cast<int32_t>(MAX_PLAYER_ORIGIN_SUBCELL);
    auto const normalizeHorizontal = [this](int32_t const value) noexcept {
        if (m_mode != WorldMode::Flight) {
            return value;
        }
        int32_t const extent = static_cast<int32_t>(
            (static_cast<int64_t>(FLIGHT_MAX_CELL) + 1) * SUBCELLS_PER_CELL
        );
        return (value % extent + extent) % extent;
    };
    int64_t const current_z = static_cast<int64_t>(player->z) * SUBCELLS_PER_CELL + player->z_subcell;
    auto const surfaceAt = [this](int64_t const x, int64_t const y, int64_t const upper_z) {
        if (m_mode != WorldMode::Flight) {
            return int64_t{ 0 };
        }
        return m_collision_world == nullptr
            ? terrainSurfaceUnderPlayer(x, y)
            : materializedTerrainSurfaceUnderPlayer(*m_collision_world, x, y, upper_z);
    };
    int64_t const gravity_step = static_cast<int64_t>(elapsed.count())
        * PLAYER_GRAVITY_SUBCELLS_PER_TICK / TICK.count();
    auto const horizontalIntervalsOverlap = [this](
        int64_t const first,
        int64_t const second,
        int64_t const width
    ) noexcept {
        return m_mode == WorldMode::Flight
            ? wrappedIntervalsOverlap(
                first,
                second,
                width,
                (static_cast<int64_t>(FLIGHT_MAX_CELL) + 1) * SUBCELLS_PER_CELL
            )
            : linearIntervalsOverlap(first, second, width);
    };
    auto const supportAt = [this, &horizontalIntervalsOverlap, &surfaceAt](
        int64_t const x,
        int64_t const y,
        int64_t const upper_z,
        PlayerId const ignored_id
    ) {
        int64_t support = surfaceAt(x, y, upper_z);
        int64_t const collision_width = m_mode == WorldMode::Flight
            ? PLAYER_WIDTH_SUBCELLS
            : static_cast<int64_t>(PLAYER_FOOTPRINT_CELLS) * SUBCELLS_PER_CELL;
        for (Player const& other : m_players) {
            if (other.id == ignored_id) {
                continue;
            }
            int64_t const other_x = static_cast<int64_t>(other.x) * SUBCELLS_PER_CELL + other.x_subcell;
            int64_t const other_y = static_cast<int64_t>(other.y) * SUBCELLS_PER_CELL + other.y_subcell;
            int64_t const other_z = static_cast<int64_t>(other.z) * SUBCELLS_PER_CELL + other.z_subcell;
            if (horizontalIntervalsOverlap(x, other_x, collision_width)
                && horizontalIntervalsOverlap(y, other_y, collision_width)) {
                int64_t const other_top = other_z + PLAYER_HEIGHT_SUBCELLS;
                if (other_top <= upper_z) {
                    support = std::max(support, other_top);
                }
            }
        }
        return support;
    };
    int64_t const current_surface = supportAt(position_x, position_y, current_z, id);
    bool const grounded = player->vertical_velocity_subcells <= 0 && current_z <= current_surface;
    if (m_mode == WorldMode::Flight && direction_z > 0 && grounded) {
        player->vertical_velocity_subcells = PLAYER_JUMP_IMPULSE_SUBCELLS;
    }
    int64_t const vertical_velocity = player->vertical_velocity_subcells;
    int64_t const elapsed_count = elapsed.count();
    int64_t const tick_count = TICK.count();
    int64_t const vertical_displacement = vertical_velocity != 0 || current_z > current_surface
        ? vertical_velocity * elapsed_count / tick_count
            - static_cast<int64_t>(PLAYER_GRAVITY_SUBCELLS_PER_TICK) * elapsed_count * elapsed_count
                / (2 * tick_count * tick_count)
        : 0;
    int64_t const proposed_z = current_z + vertical_displacement;
    bool applied_x = false;
    bool applied_y = false;
    int32_t const candidate_x = normalizeHorizontal(position_x + delta_x);
    if (delta_x != 0 && candidate_x >= 0 && candidate_x <= maximum_horizontal_subcell
        && canPlayerBeAt(static_cast<uint32_t>(candidate_x), static_cast<uint32_t>(position_y),
            proposed_z, id)
        && (m_mode != WorldMode::Flight || m_collision_world == nullptr
            || canFlightPlayerBeAt(*player, candidate_x, position_y, proposed_z))
        && proposed_z >= surfaceAt(candidate_x, position_y, proposed_z)) {
        position_x = candidate_x;
        applied_x = true;
    }
    int32_t const candidate_y = normalizeHorizontal(position_y + delta_y);
    if (delta_y != 0 && candidate_y >= 0 && candidate_y <= maximum_horizontal_subcell
        && canPlayerBeAt(static_cast<uint32_t>(position_x), static_cast<uint32_t>(candidate_y),
            proposed_z, id)
        && (m_mode != WorldMode::Flight || m_collision_world == nullptr
            || canFlightPlayerBeAt(*player, position_x, candidate_y, proposed_z))
        && proposed_z >= surfaceAt(position_x, candidate_y, proposed_z)) {
        position_y = candidate_y;
        applied_y = true;
    }
    int64_t falling_z = std::max(
        supportAt(position_x, position_y, current_z, id),
        proposed_z
    );
    bool vertical_blocked = false;
    if (!canPlayerBeAt(
        static_cast<uint32_t>(position_x),
        static_cast<uint32_t>(position_y),
        falling_z,
        id
    ) || (m_mode == WorldMode::Flight && m_collision_world != nullptr
        && !canFlightPlayerBeAt(*player, position_x, position_y, falling_z))) {
        vertical_blocked = true;
        if (vertical_displacement < 0) {
            int64_t support = surfaceAt(position_x, position_y, current_z);
            int64_t const collision_width = m_mode == WorldMode::Flight
                ? PLAYER_WIDTH_SUBCELLS
                : static_cast<int64_t>(PLAYER_FOOTPRINT_CELLS) * SUBCELLS_PER_CELL;
            for (Player const& other : m_players) {
                if (other.id == id) {
                    continue;
                }
                int64_t const other_x = static_cast<int64_t>(other.x) * SUBCELLS_PER_CELL + other.x_subcell;
                int64_t const other_y = static_cast<int64_t>(other.y) * SUBCELLS_PER_CELL + other.y_subcell;
                int64_t const other_z = static_cast<int64_t>(other.z) * SUBCELLS_PER_CELL + other.z_subcell;
                int64_t const other_top = other_z + PLAYER_HEIGHT_SUBCELLS;
                if (horizontalIntervalsOverlap(position_x, other_x, collision_width)
                    && horizontalIntervalsOverlap(position_y, other_y, collision_width)
                    && other_top <= current_z) {
                    support = std::max(support, other_top);
                }
            }
            falling_z = support;
        } else if (vertical_displacement > 0) {
            int64_t ceiling = current_z;
            int64_t const collision_width = m_mode == WorldMode::Flight
                ? PLAYER_WIDTH_SUBCELLS
                : static_cast<int64_t>(PLAYER_FOOTPRINT_CELLS) * SUBCELLS_PER_CELL;
            for (Player const& other : m_players) {
                if (other.id == id) {
                    continue;
                }
                int64_t const other_x = static_cast<int64_t>(other.x) * SUBCELLS_PER_CELL + other.x_subcell;
                int64_t const other_y = static_cast<int64_t>(other.y) * SUBCELLS_PER_CELL + other.y_subcell;
                int64_t const other_z = static_cast<int64_t>(other.z) * SUBCELLS_PER_CELL + other.z_subcell;
                if (horizontalIntervalsOverlap(position_x, other_x, collision_width)
                    && horizontalIntervalsOverlap(position_y, other_y, collision_width)
                    && other_z >= current_z) {
                    ceiling = std::min(ceiling, other_z - PLAYER_HEIGHT_SUBCELLS);
                }
            }
            falling_z = std::max(current_z, ceiling);
        } else {
            falling_z = current_z;
        }
        if (!canPlayerBeAt(
            static_cast<uint32_t>(position_x),
            static_cast<uint32_t>(position_y),
            falling_z,
            id
        )) {
            falling_z = current_z;
        }
    }
    int64_t const next_vertical_velocity = vertical_blocked ? 0 : vertical_velocity - gravity_step;
    PlayerPosition const candidate = positionFromSubcells(
        position_x,
        position_y,
        static_cast<int32_t>(falling_z)
    );
    bool const grounded_after_move = vertical_blocked
        || (falling_z <= supportAt(position_x, position_y, falling_z, id) && next_vertical_velocity <= 0);
    player->vertical_velocity_subcells = grounded_after_move ? 0 : static_cast<int32_t>(next_vertical_velocity);
    bool const moved_vertically = candidate.z != player->z || candidate.z_subcell != player->z_subcell;
    player->x = candidate.x;
    player->y = candidate.y;
    player->z = candidate.z;
    player->x_subcell = candidate.x_subcell;
    player->y_subcell = candidate.y_subcell;
    player->z_subcell = candidate.z_subcell;
    return applied_x || applied_y || moved_vertically;
}

std::vector<ChunkCoordinate> World::flightCollisionChunks(
    PlayerId const id,
    Direction const direction,
    std::chrono::milliseconds const elapsed
) const
{
    std::vector<ChunkCoordinate> chunks;
    if (m_mode != WorldMode::Flight || elapsed < std::chrono::milliseconds::zero() || elapsed > TICK) {
        return chunks;
    }
    auto const player_iterator = std::ranges::find(m_players, id, &Player::id);
    if (player_iterator == m_players.end()
        || player_iterator->movement_capabilities.allows(MovementCapability::CollisionBypass)) {
        return chunks;
    }

    int32_t const direction_x = static_cast<int8_t>(direction.x);
    int32_t const direction_y = static_cast<int8_t>(direction.y);
    int32_t const direction_z = static_cast<int8_t>(direction.z);
    bool const flies = player_iterator->movement_capabilities.allows(MovementCapability::Flight);
    uint32_t const squared_direction_length = static_cast<uint32_t>(
        direction_x * direction_x + direction_y * direction_y
            + (flies ? direction_z * direction_z : 0)
    );
    uint32_t direction_length = 0U;
    while (direction_length * direction_length < squared_direction_length) {
        ++direction_length;
    }
    uint32_t const divisor = std::max<uint32_t>(127U, direction_length);
    uint32_t const base_step = static_cast<uint32_t>(elapsed.count())
        * MOVEMENT_SUBCELLS_PER_TICK / static_cast<uint32_t>(TICK.count());
    uint32_t const acceleration = flies && direction.accelerated && isFlightSpeedupProfile(direction.speedup)
        ? direction.speedup
        : 1U;
    auto const movementDelta = [base_step, divisor, acceleration](int32_t const component) {
        return static_cast<int64_t>(base_step * static_cast<uint32_t>(std::abs(component)) / divisor * acceleration)
            * (component < 0 ? -1 : 1);
    };
    int64_t const start_x = static_cast<int64_t>(player_iterator->x) * SUBCELLS_PER_CELL
        + player_iterator->x_subcell;
    int64_t const start_y = static_cast<int64_t>(player_iterator->y) * SUBCELLS_PER_CELL
        + player_iterator->y_subcell;
    int64_t const delta_x = movementDelta(direction_x);
    int64_t const delta_y = movementDelta(direction_y);
    int64_t delta_z = movementDelta(direction_z);
    int64_t falling_delta = delta_z;
    if (!flies) {
        int64_t const velocity = player_iterator->vertical_velocity_subcells;
        int64_t const elapsed_count = elapsed.count();
        int64_t const tick_count = TICK.count();
        falling_delta = velocity * elapsed_count / tick_count
            - static_cast<int64_t>(PLAYER_GRAVITY_SUBCELLS_PER_TICK) * elapsed_count * elapsed_count
                / (2 * tick_count * tick_count);
        delta_z = direction_z > 0 ? std::max<int64_t>(falling_delta, PLAYER_JUMP_IMPULSE_SUBCELLS)
            : falling_delta;
    }
    auto const addChunk = [&chunks](ChunkCoordinate const coordinate) {
        if (std::ranges::find(chunks, coordinate) == chunks.end()) {
            chunks.push_back(coordinate);
        }
    };
    int64_t const start_z = static_cast<int64_t>(player_iterator->z) * SUBCELLS_PER_CELL
        + player_iterator->z_subcell;
    auto const addTerrainAt = [&addChunk, this](
        int64_t const x_subcells, int64_t const y_subcells, int64_t const z_subcells
    ) {
        constexpr int64_t LAST_SUBCELL_OFFSET = static_cast<int64_t>(PLAYER_WIDTH_SUBCELLS) - 1;
        int64_t const first_x = floorDivide(x_subcells, SUBCELLS_PER_CELL);
        int64_t const last_x = floorDivide(x_subcells + LAST_SUBCELL_OFFSET, SUBCELLS_PER_CELL);
        int64_t const first_y = floorDivide(y_subcells, SUBCELLS_PER_CELL);
        int64_t const last_y = floorDivide(y_subcells + LAST_SUBCELL_OFFSET, SUBCELLS_PER_CELL);
        for (int64_t cell_x = first_x; cell_x <= last_x; ++cell_x) {
            for (int64_t cell_y = first_y; cell_y <= last_y; ++cell_y) {
                int32_t const wrapped_x = wrapFlightCell(cell_x);
                int32_t const wrapped_y = wrapFlightCell(cell_y);
                if (m_collision_world != nullptr) {
                    int64_t const first_z = std::max<int64_t>(0, floorDivide(z_subcells, SUBCELLS_PER_CELL));
                    int64_t const last_z = std::min<int64_t>(FLIGHT_MAX_Z, floorDivide(
                        z_subcells + PLAYER_HEIGHT_SUBCELLS - 1, SUBCELLS_PER_CELL
                    ));
                    for (int64_t cell_z = first_z; cell_z <= last_z; ++cell_z) {
                        addChunk({
                            .x = wrapped_x / Chunk::SIDE_LENGTH,
                            .y = wrapped_y / Chunk::SIDE_LENGTH,
                            .z = static_cast<int32_t>(cell_z / Chunk::SIDE_LENGTH),
                        });
                    }
                } else {
                    uint16_t const height = collisionTerrain().heightAt(wrapped_x, wrapped_y);
                    uint16_t const terrain_z = height == 0U ? 0U : static_cast<uint16_t>(height - 1U);
                    addChunk({
                        .x = wrapped_x / Chunk::SIDE_LENGTH,
                        .y = wrapped_y / Chunk::SIDE_LENGTH,
                        .z = static_cast<int32_t>(terrain_z / Chunk::SIDE_LENGTH),
                    });
                }
            }
        }
    };
    constexpr int64_t SWEEP_STRIDE = SUBCELLS_PER_CELL / 2;
    auto const absolute = [](int64_t const value) noexcept {
        return value < 0 ? -value : value;
    };
    auto const addSweep = [&addTerrainAt, &absolute, start_z](
        int64_t const from_x,
        int64_t const from_y,
        int64_t const sweep_x,
        int64_t const sweep_y,
        int64_t const sweep_z
    ) {
        int64_t const sweep_steps = std::max<int64_t>(
            1,
            (std::max({absolute(sweep_x), absolute(sweep_y), absolute(sweep_z)}) + SWEEP_STRIDE - 1)
                / SWEEP_STRIDE
        );
        for (int64_t step = 1; step <= sweep_steps; ++step) {
            addTerrainAt(
                from_x + sweep_x * step / sweep_steps,
                from_y + sweep_y * step / sweep_steps,
                start_z + sweep_z * step / sweep_steps
            );
        }
    };

    addTerrainAt(start_x, start_y, start_z);
    addSweep(start_x, start_y, delta_x, delta_y, delta_z);
    addSweep(start_x, start_y, delta_x, 0, delta_z);
    addSweep(start_x, start_y, 0, delta_y, delta_z);
    addTerrainAt(start_x + delta_x, start_y + delta_y, start_z + delta_z);
    if (falling_delta != delta_z) {
        addSweep(start_x, start_y, delta_x, delta_y, falling_delta);
        addSweep(start_x, start_y, delta_x, 0, falling_delta);
        addSweep(start_x, start_y, 0, delta_y, falling_delta);
        addTerrainAt(start_x + delta_x, start_y + delta_y, start_z + falling_delta);
    }
    return chunks;
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
            player.vertical_velocity_subcells = 0;
            return true;
        }
    }
    return false;
}

bool World::setPlayerPaletteIndex(PlayerId const id, PlayerPaletteIndex const palette_index) noexcept
{
    if (!isValidPlayerPaletteIndex(palette_index)) {
        return false;
    }
    for (Player& player : m_players) {
        if (player.id == id) {
            player.palette_index = palette_index;
            return true;
        }
    }
    return false;
}

bool World::canSetPlayerMovementCapabilities(
    PlayerId const id,
    MovementCapabilities const capabilities
) const noexcept
{
    if (!isValidMovementCapabilities(capabilities)) {
        return false;
    }
    for (Player const& player : m_players) {
        if (player.id == id) {
            if (m_mode == WorldMode::Flight && !capabilities.allows(MovementCapability::CollisionBypass)) {
                int64_t const x = static_cast<int64_t>(player.x) * SUBCELLS_PER_CELL + player.x_subcell;
                int64_t const y = static_cast<int64_t>(player.y) * SUBCELLS_PER_CELL + player.y_subcell;
                int64_t const z = static_cast<int64_t>(player.z) * SUBCELLS_PER_CELL + player.z_subcell;
                if (m_collision_world == nullptr) {
                    if (!canFlightPlayerBeAt(player, x, y, z)) {
                        return false;
                    }
                } else {
                    if (materializedBodyIsBlocked(*m_collision_world, x, y, z, UnknownCollisionPolicy::Ignore)
                        || !canPlayerBeAt(static_cast<uint32_t>(x), static_cast<uint32_t>(y), z, id)) {
                        return false;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool World::setPlayerMovementCapabilities(
    PlayerId const id,
    MovementCapabilities const capabilities
) noexcept
{
    if (!canSetPlayerMovementCapabilities(id, capabilities)) {
        return false;
    }
    for (Player& player : m_players) {
        if (player.id == id) {
            player.vertical_velocity_subcells = 0;
            player.movement_capabilities = capabilities;
            return true;
        }
    }
    return false;
}

bool World::setPlayerVerticalVelocity(
    PlayerId const id,
    int32_t const vertical_velocity_subcells
) noexcept
{
    for (Player& player : m_players) {
        if (player.id == id) {
            player.vertical_velocity_subcells = vertical_velocity_subcells;
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

bool World::canPlayerBeAt(uint32_t const x, uint32_t const y, int64_t const z, PlayerId const id) const
{
    uint32_t const maximum_horizontal_subcell = m_mode == WorldMode::Flight
        ? static_cast<uint32_t>(FLIGHT_MAX_CELL + 1) * SUBCELLS_PER_CELL - 1U
        : MAX_PLAYER_ORIGIN_SUBCELL;
    uint32_t const collision_width = m_mode == WorldMode::Flight
        ? PLAYER_WIDTH_SUBCELLS
        : static_cast<uint32_t>(PLAYER_FOOTPRINT_CELLS) * SUBCELLS_PER_CELL;
    if (x > maximum_horizontal_subcell || y > maximum_horizontal_subcell) {
        return false;
    }
    int64_t const horizontal_period = (static_cast<int64_t>(FLIGHT_MAX_CELL) + 1) * SUBCELLS_PER_CELL;
    for (Player const& player : m_players) {
        if (player.id != id) {
            uint32_t const player_x = static_cast<uint32_t>(player.x) * SUBCELLS_PER_CELL + player.x_subcell;
            uint32_t const player_y = static_cast<uint32_t>(player.y) * SUBCELLS_PER_CELL + player.y_subcell;
            int64_t const player_z = static_cast<int64_t>(player.z) * SUBCELLS_PER_CELL + player.z_subcell;
            bool const overlaps_x = m_mode == WorldMode::Flight
                ? wrappedIntervalsOverlap(x, player_x, collision_width, horizontal_period)
                : linearIntervalsOverlap(x, player_x, collision_width);
            bool const overlaps_y = m_mode == WorldMode::Flight
                ? wrappedIntervalsOverlap(y, player_y, collision_width, horizontal_period)
                : linearIntervalsOverlap(y, player_y, collision_width);
            bool const overlaps_z = z < player_z + PLAYER_HEIGHT_SUBCELLS && player_z < z + PLAYER_HEIGHT_SUBCELLS;
            if (overlaps_x && overlaps_y && overlaps_z) {
                return false;
            }
        }
    }
    return true;
}

bool World::canFlightPlayerBeAt(
    Player const& player,
    int64_t const x_subcells,
    int64_t const y_subcells,
    int64_t const z_subcells
) const
{
    PlayerPosition const position = positionFromSubcells(
        static_cast<int32_t>(x_subcells),
        static_cast<int32_t>(y_subcells),
        static_cast<int32_t>(z_subcells)
    );
    PlayerPosition normalized = position;
    normalized.x = wrapFlightCell(normalized.x);
    normalized.y = wrapFlightCell(normalized.y);
    if (!isFlightPositionInBounds(normalized)) {
        return false;
    }
    if (m_collision_world != nullptr) {
        if (materializedBodyIsBlocked(
                *m_collision_world,
                x_subcells,
                y_subcells,
                z_subcells,
                UnknownCollisionPolicy::Block
            )) {
            return false;
        }
    } else if (isInsideTerrain(x_subcells, y_subcells, z_subcells)) {
        return false;
    }
    return canPlayerBeAt(
        static_cast<uint32_t>(normalized.x * SUBCELLS_PER_CELL + normalized.x_subcell),
        static_cast<uint32_t>(normalized.y * SUBCELLS_PER_CELL + normalized.y_subcell),
        z_subcells,
        player.id
    );
}

bool World::isFlightPositionInBounds(PlayerPosition const position) noexcept
{
    return position.x >= FLIGHT_MIN_CELL && position.x <= FLIGHT_MAX_CELL
        && position.y >= FLIGHT_MIN_CELL && position.y <= FLIGHT_MAX_CELL
        && position.z >= FLIGHT_MIN_CELL && position.z <= FLIGHT_MAX_Z
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
    uint32_t const acceleration = direction.accelerated && isFlightSpeedupProfile(direction.speedup)
        ? direction.speedup
        : 1U;
    auto const movementDelta = [base_step, divisor, acceleration](int32_t const component) {
        return static_cast<int32_t>(base_step * static_cast<uint32_t>(std::abs(component)) / divisor * acceleration)
            * (component < 0 ? -1 : 1);
    };
    int64_t const start_x = static_cast<int64_t>(player.x) * SUBCELLS_PER_CELL + player.x_subcell;
    int64_t const start_y = static_cast<int64_t>(player.y) * SUBCELLS_PER_CELL + player.y_subcell;
    int64_t const start_z = static_cast<int64_t>(player.z) * SUBCELLS_PER_CELL + player.z_subcell;
    int64_t const delta_x = movementDelta(direction_x);
    int64_t const delta_y = movementDelta(direction_y);
    int64_t const delta_z = movementDelta(direction_z);
    int64_t target_z = start_z + delta_z;
    bool const collision_bypass = player.movement_capabilities.allows(MovementCapability::CollisionBypass);
    if (!collision_bypass && m_collision_world == nullptr && delta_z < 0 && delta_x == 0 && delta_y == 0) {
        target_z = std::max(target_z, terrainSurfaceUnderPlayer(start_x, start_y));
    }

    auto const absolute = [](int64_t const value) noexcept {
        return value < 0 ? -value : value;
    };
    constexpr int64_t SWEEP_STRIDE = SUBCELLS_PER_CELL / 2;
    int64_t resolved_x = start_x + delta_x;
    int64_t resolved_y = start_y + delta_y;
    int64_t resolved_z = target_z;
    if (!collision_bypass) {
        auto const sweepClear = [&](int64_t const from_x,
            int64_t const from_y,
            int64_t const from_z,
            int64_t const sweep_x,
            int64_t const sweep_y,
            int64_t const sweep_z) {
            int64_t const sweep_steps = std::max<int64_t>(
                1,
                (std::max({ absolute(sweep_x), absolute(sweep_y), absolute(sweep_z) })
                    + SWEEP_STRIDE - 1) / SWEEP_STRIDE
            );
            for (int64_t step = 1; step <= sweep_steps; ++step) {
                int64_t const sample_x = from_x + sweep_x * step / sweep_steps;
                int64_t const sample_y = from_y + sweep_y * step / sweep_steps;
                int64_t const sample_z = from_z + sweep_z * step / sweep_steps;
                if (!canFlightPlayerBeAt(player, sample_x, sample_y, sample_z)) {
                    return false;
                }
            }
            return true;
        };

        // Check the complete three-axis trajectory first.  If a diagonal flight
        // path crosses another body or a ridge while descending, checking only
        // the start height and the final horizontal position would tunnel through
        // the obstruction.  When the direct trajectory is blocked, the axis
        // sweeps below retain the expected tangential wall sliding behaviour.
        bool const direct_path_clear = sweepClear(start_x, start_y, start_z, delta_x, delta_y, delta_z);
        if (direct_path_clear) {
            resolved_x = start_x + delta_x;
            resolved_y = start_y + delta_y;
            resolved_z = target_z;
        }

        struct HorizontalCandidate final {
            int64_t x;
            int64_t y;
        };
        auto const resolveHorizontal = [&](int64_t const delta, bool const on_x_axis) {
            HorizontalCandidate candidate{ .x = start_x, .y = start_y };
            if (delta == 0) {
                return candidate;
            }
            int64_t const axis_steps = std::max<int64_t>(
                1,
                (absolute(delta) + SWEEP_STRIDE - 1) / SWEEP_STRIDE
            );
            for (int64_t step = 1; step <= axis_steps; ++step) {
                int64_t const sample_x = on_x_axis
                    ? start_x + delta * step / axis_steps : start_x;
                int64_t const sample_y = on_x_axis
                    ? start_y : start_y + delta * step / axis_steps;
                int64_t const sample_z = start_z + delta_z * step / axis_steps;
                if (!canFlightPlayerBeAt(player, sample_x, sample_y, sample_z)) {
                    break;
                }
                candidate.x = sample_x;
                candidate.y = sample_y;
            }
            return candidate;
        };

        if (!direct_path_clear) {
            HorizontalCandidate const x_candidate = resolveHorizontal(delta_x, true);
            HorizontalCandidate const y_candidate = resolveHorizontal(delta_y, false);
            if (delta_x == 0) {
                resolved_x = y_candidate.x;
                resolved_y = y_candidate.y;
            } else if (delta_y == 0) {
                resolved_x = x_candidate.x;
                resolved_y = x_candidate.y;
            } else {
                // Never compose two independently clear axis paths after the
                // combined diagonal was blocked: that L-shaped fallback can
                // route around a corner-only body intersection.  Select the
                // axis that preserves the most tangential progress instead.
                int64_t const x_progress = absolute(x_candidate.x - start_x);
                int64_t const y_progress = absolute(y_candidate.y - start_y);
                HorizontalCandidate const selected = x_progress >= y_progress
                    ? x_candidate : y_candidate;
                resolved_x = selected.x;
                resolved_y = selected.y;
            }
        }

        if (delta_z != 0) {
            int64_t const starting_surface = m_collision_world == nullptr
                ? terrainSurfaceUnderPlayer(resolved_x, resolved_y) : 0;
            int64_t const vertical_steps = std::max<int64_t>(
                1,
                (absolute(delta_z) + SWEEP_STRIDE - 1) / SWEEP_STRIDE
            );
            int64_t last_z = start_z;
            for (int64_t step = 1; step <= vertical_steps; ++step) {
                int64_t sample_z = start_z + delta_z * step / vertical_steps;
                int64_t const surface = m_collision_world == nullptr
                    ? terrainSurfaceUnderPlayer(resolved_x, resolved_y) : 0;
                if (sample_z < surface) {
                    if (delta_z > 0) {
                        // The player may have advanced onto a higher terrain
                        // column during the horizontal sweep. Keep ascending
                        // until the body clears that column instead of freezing
                        // at the old height.
                        continue;
                    }
                    if (surface <= starting_surface) {
                        last_z = surface;
                    }
                    break;
                }
                if (!canFlightPlayerBeAt(player, resolved_x, resolved_y, sample_z)) {
                    break;
                }
                last_z = sample_z;
            }
            resolved_z = last_z;
        }
    }
    PlayerPosition candidate = positionFromSubcells(
        static_cast<int32_t>(resolved_x),
        static_cast<int32_t>(resolved_y),
        static_cast<int32_t>(resolved_z)
    );
    candidate.x = wrapFlightCell(candidate.x);
    candidate.y = wrapFlightCell(candidate.y);
    if (!isFlightPositionInBounds(candidate)) {
        return false;
    }
    if (!collision_bypass && !canFlightPlayerBeAt(
        player,
        resolved_x,
        resolved_y,
        resolved_z
    )) {
        return false;
    }
    bool const changed = candidate.x != player.x
        || candidate.y != player.y
        || candidate.z != player.z
        || candidate.x_subcell != player.x_subcell
        || candidate.y_subcell != player.y_subcell
        || candidate.z_subcell != player.z_subcell;
    if (!changed) {
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
