#include <server/GameServer.hpp>

#include <server/HeightTileWorkerPool.hpp>
#include <server/WorldPreparation.hpp>
#include <server/detail/NetworkEventBatch.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>
#include <core/common/Assert.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>
#include <core/net/Peer.hpp>
#include <core/net/SendMode.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>

namespace {

[[nodiscard]]
shared::PolicyDiagnostic policyDiagnostic(
    shared::PolicyDiagnosticCode const code,
    std::string source_id,
    std::string message
)
{
    return {
        .code = code,
        .source_id = std::move(source_id),
        .message = std::move(message),
    };
}

[[nodiscard]]
std::optional<shared::PolicyCapabilityKeyId> capabilityKey(
    shared::PolicyCapabilityRegistry const& registry,
    std::string_view const key
) noexcept
{
    for (shared::PolicyCapabilityKeyId index = 0U; index < registry.definitions.size(); ++index) {
        if (registry.definitions[index].key == key) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]]
int32_t floorDivideByHeightTileSide(int32_t const value) noexcept
{
    int32_t result = value / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    if (value < 0 && value % static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) != 0) {
        --result;
    }
    return result;
}

[[nodiscard]]
shared::HeightTileKey heightTileKeyForCoordinates(int32_t const x, int32_t const y) noexcept
{
    return shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(x),
        .y = floorDivideByHeightTileSide(y),
    });
}

[[nodiscard]]
shared::HeightTileKey heightTileKeyForPlayer(shared::Player const& player) noexcept
{
    return heightTileKeyForCoordinates(player.x, player.y);
}

std::shared_ptr<shared::TerrainWorld const> checkedTerrainWorld(
    shared::WorldConfiguration const& configuration,
    std::shared_ptr<shared::TerrainWorld const> terrain_world
)
{
    if (terrain_world != nullptr) {
        shared::TerrainIdentity const& identity = terrain_world->identity();
        if (!shared::isValidTerrainIdentity(identity)
            || identity.seed != configuration.seed
            || identity.revision != configuration.algorithm_version) {
            throw std::invalid_argument{ "prepared terrain identity does not match the server world configuration" };
        }
    }
    return terrain_world;
}

std::unique_ptr<shared::SparseWorld> makePhysicsWorld(
    shared::SparseWorldOptions options,
    std::shared_ptr<shared::TerrainWorld const> const& terrain_world
)
{
    if (terrain_world != nullptr) {
        return std::make_unique<shared::SparseWorld>(options, terrain_world);
    }
    return std::make_unique<shared::SparseWorld>(options);
}

struct ChunkCoordinateHash final {
    [[nodiscard]] size_t operator()(shared::ChunkCoordinate const coordinate) const noexcept
    {
        uint64_t hash = 14'695'981'039'346'656'037ULL;
        for (int32_t const component : { coordinate.x, coordinate.y, coordinate.z }) {
            hash ^= static_cast<uint32_t>(component);
            hash *= 1'099'511'628'211ULL;
        }
        return static_cast<size_t>(hash);
    }
};

using ChunkKeySet = std::unordered_set<shared::ChunkCoordinate, ChunkCoordinateHash>;

[[nodiscard]]
std::array<shared::ChunkCoordinate, 6U> chunkNeighbors(shared::ChunkCoordinate const coordinate) noexcept
{
    static constexpr int32_t CHUNK_COUNT_XY = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH
    );
    auto const wrap = [](int32_t const value) noexcept {
        return (value % CHUNK_COUNT_XY + CHUNK_COUNT_XY) % CHUNK_COUNT_XY;
    };
    return {
        shared::ChunkCoordinate{ .x = wrap(coordinate.x + 1), .y = coordinate.y, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = wrap(coordinate.x - 1), .y = coordinate.y, .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = wrap(coordinate.y + 1), .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = wrap(coordinate.y - 1), .z = coordinate.z },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = coordinate.y, .z = coordinate.z + 1 },
        shared::ChunkCoordinate{ .x = coordinate.x, .y = coordinate.y, .z = coordinate.z - 1 },
    };
}

template <typename ChunkSet>
[[nodiscard]]
bool faceConnectedTo(
    shared::ChunkCoordinate const coordinate,
    ChunkSet const& connected,
    shared::ChunkCoordinate const root
) noexcept
{
    if (connected.empty()) {
        return coordinate == root;
    }
    auto const neighbors = chunkNeighbors(coordinate);
    return std::ranges::any_of(neighbors, [&connected](shared::ChunkCoordinate const neighbor) {
        return connected.contains(neighbor);
    });
}

template <typename ChunkSet>
[[nodiscard]]
ChunkSet rootedChunkComponent(
    ChunkSet const& candidates, // NOLINT(bugprone-easily-swappable-parameters): candidate set and root set have distinct roles.
    ChunkSet const& roots
)
{
    ChunkSet connected;
    connected.reserve(candidates.size());
    std::vector<shared::ChunkCoordinate> frontier;
    frontier.reserve(candidates.size());
    for (shared::ChunkCoordinate const root : roots) {
        if (candidates.contains(root) && connected.insert(root).second) {
            frontier.push_back(root);
        }
    }
    for (size_t head = 0U; head < frontier.size(); ++head) {
        for (shared::ChunkCoordinate const neighbor : chunkNeighbors(frontier[head])) {
            if (candidates.contains(neighbor) && connected.insert(neighbor).second) {
                frontier.push_back(neighbor);
            }
        }
    }
    return connected;
}

[[nodiscard]]
bool sameInterestDirection(
    std::optional<shared::Direction> const& first,
    std::optional<shared::Direction> const& second
) noexcept
{
    if (first.has_value() != second.has_value()) {
        return false;
    }
    if (!first.has_value()) {
        return true;
    }
    return first->x == second->x
        && first->y == second->y
        && first->z == second->z
        && first->view_x == second->view_x
        && first->view_y == second->view_y;
}

[[nodiscard]]
uint32_t postNearMaterializationPriority(
    uint32_t const index,
    uint32_t const near_count,
    bool const prefetch
) noexcept
{
    uint64_t const rank = prefetch
        ? static_cast<uint64_t>(near_count) + static_cast<uint64_t>(index) * 4U + 3U
        : static_cast<uint64_t>(near_count)
            + static_cast<uint64_t>(index / 3U) * 4U
            + index % 3U;
    return static_cast<uint32_t>(std::min<uint64_t>(rank, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]]
int64_t movementProjection(
    shared::ChunkCoordinate const center_chunk,
    std::optional<shared::Direction> const direction,
    shared::ChunkCoordinate const coordinate
) noexcept
{
    if (!direction.has_value()) {
        return 0;
    }
    static constexpr int32_t CHUNK_COUNT_XY = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH
    );
    auto const shortest_delta = [](int32_t const target, int32_t const origin) noexcept {
        int32_t delta = target - origin;
        if (delta > CHUNK_COUNT_XY / 2) {
            delta -= CHUNK_COUNT_XY;
        } else if (delta < -CHUNK_COUNT_XY / 2) {
            delta += CHUNK_COUNT_XY;
        }
        return delta;
    };
    int8_t dx = static_cast<int8_t>(direction->x);
    int8_t dy = static_cast<int8_t>(direction->y);
    int8_t const dz = static_cast<int8_t>(direction->z);
    if (dx == 0 && dy == 0 && dz == 0) {
        dx = direction->view_x;
        dy = direction->view_y;
    }
    return static_cast<int64_t>(shortest_delta(coordinate.x, center_chunk.x)) * dx
        + static_cast<int64_t>(shortest_delta(coordinate.y, center_chunk.y)) * dy
        + static_cast<int64_t>(coordinate.z - center_chunk.z) * dz;
}

void prioritizeNeighbors(
    std::array<shared::ChunkCoordinate, 6U>& neighbors,
    shared::PlayerPosition const center,
    std::optional<shared::Direction> const direction,
    ChunkKeySet const* const preferred_keys = nullptr
)
{
    if (!direction.has_value() && preferred_keys == nullptr) {
        return;
    }
    struct NeighborPriority final {
        shared::ChunkCoordinate coordinate;
        bool preferred;
        int64_t projection;
    };
    shared::ChunkCoordinate const center_chunk = shared::WorldBounds::chunkCoordinate({
        .x = static_cast<int64_t>(center.x),
        .y = static_cast<int64_t>(center.y),
        .z = static_cast<int64_t>(center.z),
    });
    std::array<NeighborPriority, 6U> priorities{};
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        shared::ChunkCoordinate const coordinate = neighbors[index];
        priorities[index] = NeighborPriority{
            .coordinate = coordinate,
            .preferred = preferred_keys != nullptr && preferred_keys->contains(coordinate),
            .projection = movementProjection(center_chunk, direction, coordinate),
        };
    }
    std::ranges::stable_sort(priorities, [](NeighborPriority const& left, NeighborPriority const& right) {
        if (left.preferred != right.preferred) {
            return left.preferred;
        }
        if (left.projection != right.projection) {
            return left.projection > right.projection;
        }
        return std::tie(left.coordinate.z, left.coordinate.y, left.coordinate.x)
            < std::tie(right.coordinate.z, right.coordinate.y, right.coordinate.x);
    });
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        neighbors[index] = priorities[index].coordinate;
    }
}

[[nodiscard]]
bool isZyxLess(shared::ChunkCoordinate const left, shared::ChunkCoordinate const right) noexcept
{
    return std::tie(left.z, left.y, left.x) < std::tie(right.z, right.y, right.x);
}

template <typename NearChunkSet, typename FullChunkSet>
[[nodiscard]]
std::vector<shared::ChunkCoordinate> connectedChunkOrder(
    shared::PlayerPosition const center,
    NearChunkSet const& near_set,
    FullChunkSet const& full_set,
    std::optional<shared::Direction> const direction
)
{
    std::vector<shared::ChunkCoordinate> order;
    order.reserve(full_set.size());
    shared::ChunkCoordinate const center_chunk = shared::WorldBounds::chunkCoordinate({
        .x = static_cast<int64_t>(center.x),
        .y = static_cast<int64_t>(center.y),
        .z = static_cast<int64_t>(center.z),
    });
    ChunkKeySet visited;
    visited.reserve(full_set.size());
    auto const appendComponent = [&](auto const& candidates, auto& component_keys, shared::ChunkCoordinate root) {
        size_t const first = order.size();
        if (component_keys.insert(root).second) {
            order.push_back(root);
        }
        for (size_t head = first; head < order.size(); ++head) {
            auto neighbors = chunkNeighbors(order[head]);
            prioritizeNeighbors(neighbors, center, direction);
            for (shared::ChunkCoordinate const neighbor : neighbors) {
                if (candidates.contains(neighbor) && component_keys.insert(neighbor).second) {
                    order.push_back(neighbor);
                }
            }
        }
    };

    ChunkKeySet near_visited;
    near_visited.reserve(near_set.size());
    if (near_set.contains(center_chunk)) {
        appendComponent(near_set, near_visited, center_chunk);
    }
    std::vector<shared::ChunkCoordinate> near_roots{ near_set.begin(), near_set.end() };
    std::sort(near_roots.begin(), near_roots.end(), isZyxLess);
    for (shared::ChunkCoordinate const root : near_roots) {
        if (!near_visited.contains(root)) {
            appendComponent(near_set, near_visited, root);
        }
    }

    // Clipped closed-sphere coverage may not contain the entity's center chunk.
    // Keep every selected key and seed any remaining component from a stable key.
    visited = near_visited;
    for (size_t head = 0U; head < order.size(); ++head) {
        auto neighbors = chunkNeighbors(order[head]);
        prioritizeNeighbors(neighbors, center, direction);
        for (shared::ChunkCoordinate const neighbor : neighbors) {
            if (full_set.contains(neighbor) && visited.insert(neighbor).second) {
                order.push_back(neighbor);
            }
        }
    }
    std::vector<shared::ChunkCoordinate> full_roots{ full_set.begin(), full_set.end() };
    std::sort(full_roots.begin(), full_roots.end(), isZyxLess);
    for (shared::ChunkCoordinate const root : full_roots) {
        if (!visited.contains(root)) {
            appendComponent(full_set, visited, root);
        }
    }
    if (near_visited.size() != near_set.size() || visited.size() != full_set.size()) {
        throw std::logic_error{ "terrain coverage ordering omitted a required chunk" };
    }
    return order;
}

template <typename CurrentChunkSet, typename InterestChunkSet>
[[nodiscard]]
std::vector<shared::ChunkCoordinate> connectedPrefetchOrder(
    shared::PlayerPosition const center,
    std::vector<shared::ChunkCoordinate> const& current_order,
    CurrentChunkSet const& current_set,
    InterestChunkSet const& interest_set,
    std::optional<shared::Direction> const direction,
    ChunkKeySet const& forecast_lead_keys
)
{
    std::vector<shared::ChunkCoordinate> lead_order;
    lead_order.reserve(interest_set.size() - current_order.size());
    ChunkKeySet visited_prefetch;
    visited_prefetch.reserve(interest_set.size() - current_order.size());
    auto append_prefetch_neighbors = [&](shared::ChunkCoordinate const coordinate) {
        auto neighbors = chunkNeighbors(coordinate);
        prioritizeNeighbors(neighbors, center, direction, &forecast_lead_keys);
        for (shared::ChunkCoordinate const neighbor : neighbors) {
            if (interest_set.contains(neighbor)
                && !current_set.contains(neighbor)
                && visited_prefetch.insert(neighbor).second) {
                lead_order.push_back(neighbor);
            }
        }
    };
    for (shared::ChunkCoordinate const coordinate : current_order) {
        append_prefetch_neighbors(coordinate);
    }
    for (size_t head = 0U; head < lead_order.size(); ++head) {
        append_prefetch_neighbors(lead_order[head]);
    }
    std::vector<shared::ChunkCoordinate> roots{ interest_set.begin(), interest_set.end() };
    std::sort(roots.begin(), roots.end(), isZyxLess);
    for (shared::ChunkCoordinate const root : roots) {
        if (current_set.contains(root) || !visited_prefetch.insert(root).second) {
            continue;
        }
        size_t const first = lead_order.size();
        lead_order.push_back(root);
        for (size_t head = first; head < lead_order.size(); ++head) {
            append_prefetch_neighbors(lead_order[head]);
        }
    }
    if (current_set.size() + visited_prefetch.size() != interest_set.size()) {
        throw std::logic_error{ "72-block materialized interest ordering omitted a required chunk" };
    }
    return lead_order;
}

template <typename FirstChunkSet, typename SecondChunkSet>
[[nodiscard]]
bool chunkSetsTouch(FirstChunkSet const& first, SecondChunkSet const& second)
{
    if (first.empty() && second.empty()) {
        return true;
    }
    for (shared::ChunkCoordinate const coordinate : first) {
        if (second.contains(coordinate)) {
            return true;
        }
        for (shared::ChunkCoordinate const neighbor : chunkNeighbors(coordinate)) {
            if (second.contains(neighbor)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]]
shared::MaterializedChunkPayload materializedChunkPayload(shared::Chunk const& chunk)
{
    shared::Chunk::Blocks blocks{};
    std::optional<shared::Block> const first = chunk.blockAt({ .x = 0U, .y = 0U, .z = 0U });
    ASSERT(first.has_value(), "chunk origin must be a valid block coordinate");
    bool uniform = true;
    for (uint8_t z = 0U; z < shared::Chunk::SIDE_LENGTH; ++z) {
        for (uint8_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
                uint32_t const index = static_cast<uint32_t>(z) * shared::Chunk::FACE_BLOCK_COUNT
                    + static_cast<uint32_t>(y) * shared::Chunk::SIDE_LENGTH
                    + x;
                std::optional<shared::Block> const block = chunk.blockAt({ .x = x, .y = y, .z = z });
                ASSERT(block.has_value(), "loop coordinates must identify a valid block in the chunk");
                blocks[index] = *block;
                uniform = uniform && blocks[index] == *first;
            }
        }
    }
    shared::MaterializedChunkPayload payload{
        .coordinate = chunk.coordinate(),
        .content_hash = chunk.contentIdentity().content_hash,
    };
    if (uniform) {
        payload.data = shared::UniformChunkData{ .material = *first };
    } else {
        payload.data = shared::DenseChunkData{ .blocks = blocks };
    }
    return payload;
}

[[nodiscard]]
bool samePosition(shared::PlayerPosition const first, shared::PlayerPosition const second) noexcept
{
    return first.x == second.x && first.y == second.y && first.z == second.z
        && first.x_subcell == second.x_subcell && first.y_subcell == second.y_subcell
        && first.z_subcell == second.z_subcell;
}

constexpr int32_t HEIGHT_TILE_COUNT = static_cast<int32_t>(
    shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
);
constexpr uint32_t NEAR_START_RADIUS_BLOCKS = shared::NEAR_TERRAIN_RADIUS_BLOCKS;

[[nodiscard]]
shared::TerrainSessionIdentity terrainSessionIdentity(
    uint64_t const join_epoch,
    shared::WorldConfiguration const& world_configuration,
    generation::GenerationConfiguration const& generation_configuration
)
{
    return {
        .join_epoch = join_epoch,
        .terrain = shared::TerrainIdentity{
            .terrain_version = generation::TERRAIN_VERSION,
            .revision = world_configuration.algorithm_version,
            .seed = generation_configuration.seed,
            .configuration_identity = generation::configurationIdentity(generation_configuration),
        },
        .configuration = generation_configuration,
    };
}

[[nodiscard]]
std::string boundedFailure(std::string message)
{
    static constexpr size_t MAX_FAILURE_BYTES = 512U;
    if (message.empty()) {
        return "Terrain world preparation failed.";
    }
    if (message.size() > MAX_FAILURE_BYTES) {
        message.resize(MAX_FAILURE_BYTES);
    }
    return message;
}
constexpr int32_t CURRENT_NEAR_WINDOW_RADIUS = 7;
constexpr uint64_t SERVER_TERRAIN_WORLD_EPOCH = 1U;

[[nodiscard]]
shared::WorldGenerationPlan typedTerrainPlan(std::vector<shared::GenerationStage> chunk_stages)
{
    return {
        .pre_generate = true,
        .refinement_extents = {4'096U, 1'024U, 256U, 64U},
        .chunk_stages = std::move(chunk_stages),
    };
}

[[nodiscard]]
bool hasCompleteTypedTerrainPlan(shared::WorldGenerationPlan const& plan) noexcept
{
    static constexpr std::array<uint32_t, 4U> REQUIRED_REFINEMENT_EXTENTS{
        4'096U,
        1'024U,
        256U,
        64U,
    };
    return plan.pre_generate
        && std::ranges::equal(plan.refinement_extents, REQUIRED_REFINEMENT_EXTENTS)
        && !plan.chunk_stages.empty()
        && shared::isValidWorldGenerationPlan(plan);
}

[[nodiscard]]
bool isCanonicalHeightTile(
    shared::HeightTile const& tile,
    shared::TerrainIdentity const& identity,
    shared::ChunkCoordinate const coordinate
) noexcept
{
    return tile.coordinate == shared::HeightTileCoordinate{.x = coordinate.x, .y = coordinate.y}
        && tile.surface.coordinate == shared::ChunkCoordinate{.x = coordinate.x, .y = coordinate.y, .z = 0}
        && tile.surface.identity == identity
        && shared::isValidSurfaceTile(tile.surface)
        && std::ranges::equal(tile.heights, tile.surface.columns, [](uint16_t const height,
            shared::TerrainColumn const& column) {
            return height == column.highest_solid_z + 1U;
        });
}

[[nodiscard]]
int32_t shortestWrappedHeightTileDelta(int32_t const target, int32_t const center) noexcept
{
    int32_t delta = target - center;
    if (delta > HEIGHT_TILE_COUNT / 2) {
        delta -= HEIGHT_TILE_COUNT;
    } else if (delta < -HEIGHT_TILE_COUNT / 2) {
        delta += HEIGHT_TILE_COUNT;
    }
    return delta;
}

[[nodiscard]]
bool isCurrentNearHeightTile(
    shared::HeightTileKey const center,
    shared::HeightTileKey const key
) noexcept
{
    int32_t const delta_x = shortestWrappedHeightTileDelta(key.x, center.x);
    int32_t const delta_y = shortestWrappedHeightTileDelta(key.y, center.y);
    return delta_x >= -CURRENT_NEAR_WINDOW_RADIUS && delta_x <= CURRENT_NEAR_WINDOW_RADIUS
        && delta_y >= -CURRENT_NEAR_WINDOW_RADIUS && delta_y <= CURRENT_NEAR_WINDOW_RADIUS;
}

template <typename IsDesired, typename IsAvailable>
[[nodiscard]]
bool nearWindowHasUntrackedTiles(
    shared::HeightTileKey const center,
    IsDesired const& is_desired,
    IsAvailable const& is_available
)
{
    for (int32_t y = -CURRENT_NEAR_WINDOW_RADIUS; y <= CURRENT_NEAR_WINDOW_RADIUS; ++y) {
        for (int32_t x = -CURRENT_NEAR_WINDOW_RADIUS; x <= CURRENT_NEAR_WINDOW_RADIUS; ++x) {
            shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                .x = center.x + x,
                .y = center.y + y,
            });
            if (is_desired(key) && !is_available(key)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]]
bool isForecastNearHeightTile(
    std::optional<shared::HeightTileKey> const forecast_center,
    shared::HeightTileKey const key
) noexcept
{
    return forecast_center.has_value() && isCurrentNearHeightTile(*forecast_center, key);
}

[[nodiscard]]
std::tuple<uint8_t, double, int32_t, int32_t> heightTilePriority(
    shared::HeightTileKey const center,
    std::optional<shared::HeightTileKey> const forecast_center,
    int32_t const movement_x,
    int32_t const movement_y,
    shared::HeightTileKey const key
) noexcept
{
    uint8_t const tier = isCurrentNearHeightTile(center, key)
        ? 0U
        : (isForecastNearHeightTile(forecast_center, key) ? 1U : 2U);
    return {
        tier,
        shared::heightTileInterestPriority(
            center,
            static_cast<int8_t>(movement_x),
            static_cast<int8_t>(movement_y),
            key
        ),
        key.y,
        key.x,
    };
}

[[nodiscard]]
int32_t shortestWrappedChunkDelta(int32_t const target, int32_t const center) noexcept
{
    static constexpr int32_t CHUNK_COUNT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH
    );
    static constexpr int32_t HALF_WORLD = CHUNK_COUNT / 2;
    int32_t delta = target - center;
    if (delta > HALF_WORLD) {
        delta -= CHUNK_COUNT;
    } else if (delta < -HALF_WORLD) {
        delta += CHUNK_COUNT;
    }
    return delta;
}

[[nodiscard]]
uint32_t directionalPriorityScore(int64_t const projection) noexcept
{
    static constexpr int64_t PRIORITY_BIAS = 1'000'000;
    return static_cast<uint32_t>(std::clamp(
        PRIORITY_BIAS - projection,
        int64_t{0},
        PRIORITY_BIAS * 2
    ));
}

[[nodiscard]]
shared::GenerationPriorityScores generationPriorityScores(
    shared::Player const& player,
    shared::Direction const direction,
    shared::ChunkCoordinate const coordinate
) noexcept
{
    int32_t const center_x = player.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const center_y = player.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const center_z = player.z / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    int32_t const delta_x = shortestWrappedChunkDelta(coordinate.x, center_x);
    int32_t const delta_y = shortestWrappedChunkDelta(coordinate.y, center_y);
    int32_t const delta_z = coordinate.z - center_z;
    int64_t const view_projection = static_cast<int64_t>(delta_x) * direction.view_x
        + static_cast<int64_t>(delta_y) * direction.view_y;
    int64_t const movement_projection = static_cast<int64_t>(delta_x) * static_cast<int8_t>(direction.x)
        + static_cast<int64_t>(delta_y) * static_cast<int8_t>(direction.y)
        + static_cast<int64_t>(delta_z) * static_cast<int8_t>(direction.z);
    return {
        .distance = static_cast<uint32_t>(std::abs(delta_x) + std::abs(delta_y) + std::abs(delta_z)),
        .view = directionalPriorityScore(view_projection),
        .movement = directionalPriorityScore(movement_projection),
    };
}

[[nodiscard]]
uint64_t nextHeightTileToken(uint64_t& next_token) noexcept
{
    uint64_t const token = next_token;
    ++next_token;
    if (next_token == 0U) {
        next_token = 1U;
    }
    return token;
}


} // namespace

namespace server {

size_t GameServer::PlayerReplication::ChunkCoordinateHash::operator()(
    shared::ChunkCoordinate coordinate
) const noexcept
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (int32_t const component : { coordinate.x, coordinate.y, coordinate.z }) {
        hash ^= static_cast<uint32_t>(component);
        hash *= 1'099'511'628'211ULL;
    }
    return static_cast<size_t>(hash);
}

GameServer::GameServer(
    uint16_t const port,
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode,
    shared::WorldConfiguration const configuration,
    uint32_t const render_distance,
    std::shared_ptr<shared::TerrainWorld const> terrain_world,
    std::shared_ptr<WorldPreparation> world_preparation,
    BindMode const bind_mode
)
    : core::Server{bind_mode == BindMode::Loopback
            ? core::Address::localhost(port) : core::Address::anyhost(port), 4, 2}
    , m_world{world_mode, configuration}
    , m_terrain_world{checkedTerrainWorld(configuration, std::move(terrain_world))}
    , m_world_preparation{std::move(world_preparation)}
    , m_physics_world{m_world_preparation == nullptr
            ? makePhysicsWorld(shared::SparseWorldOptions{
                .seed = configuration.seed,
                .revision = configuration.algorithm_version,
            }, m_terrain_world)
            : nullptr}
    , m_world_generation{192U, configuration.algorithm_version, configuration.seed}
    , m_terrain_generator{m_world_preparation == nullptr && m_terrain_world == nullptr
            ? std::make_shared<shared::TerrainGenerator const>()
            : nullptr}
    , m_generation_plan{m_world_preparation != nullptr
            ? shared::WorldGenerationPlan{}
            : (m_terrain_world != nullptr
                ? typedTerrainPlan({shared::GenerationStage::Materialize})
                : m_terrain_generator->generationPlan())}
    , m_spawn_points{checkedSpawnPoints(std::move(spawn_points), world_mode)}
    , m_height_tile_interest_orders{shared::prepareHeightTileInterestOrders(render_distance)}
{
    if (m_world_preparation != nullptr && world_mode != shared::WorldMode::Flight) {
        throw std::invalid_argument{ "prepared terrain sessions require flight mode" };
    }
    if (m_world_preparation != nullptr
        && (m_world_preparation->configuration().seed != configuration.seed
            || m_world_preparation->configuration().seed != m_world.configuration().seed)) {
        throw std::invalid_argument{ "world preparation seed does not match server world configuration" };
    }
    if (m_world_preparation == nullptr) {
        m_world.setCollisionWorld(m_physics_world.get());
        if (m_terrain_world != nullptr) {
            m_world_generation.setTerrainContext(m_terrain_world, SERVER_TERRAIN_WORLD_EPOCH);
        }
        uint32_t const worker_count = terrainWorkerCount(std::thread::hardware_concurrency());
        if (m_terrain_world != nullptr) {
            m_height_tile_workers = std::make_unique<HeightTileWorkerPool>(m_terrain_world, worker_count);
        } else {
            m_height_tile_workers = std::make_unique<HeightTileWorkerPool>(m_terrain_generator, worker_count);
        }
    }
}

GameServer::~GameServer() = default;

void GameServer::populateNearWindowMetrics(
    PreviewStream const& stream,
    PreviewStreamMetrics& metrics
) const
{
    metrics.terrain_center_valid = stream.has_center;
    if (!stream.has_center) {
        return;
    }
    static constexpr int32_t NEAR_WINDOW_RADIUS = 7;
    metrics.terrain_center_x = stream.center.x;
    metrics.terrain_center_y = stream.center.y;
    for (int32_t y = -NEAR_WINDOW_RADIUS; y <= NEAR_WINDOW_RADIUS; ++y) {
        for (int32_t x = -NEAR_WINDOW_RADIUS; x <= NEAR_WINDOW_RADIUS; ++x) {
            shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                .x = stream.center.x + x,
                .y = stream.center.y + y,
            });
            if (!stream.desires(key)) {
                continue;
            }
            ++metrics.near_window_desired_tiles;
            if (stream.resident_keys.contains(key)) {
                ++metrics.near_window_resident_tiles;
            } else if (stream.queued_keys.contains(key)) {
                ++metrics.near_window_queued_tiles;
            } else if (stream.dispatched_keys.contains(key)) {
                ++metrics.near_window_dispatched_tiles;
            } else if (stream.ready_tiles.contains(key)) {
                ++metrics.near_window_ready_tiles;
            } else if (stream.inflight_addition_keys.contains(key)) {
                ++metrics.near_window_inflight_additions;
            } else {
                ++metrics.near_window_untracked_tiles;
            }
            if (stream.inflight_removal_keys.contains(key)) {
                ++metrics.near_window_inflight_removals;
            }
        }
    }
}

std::expected<uint64_t, shared::PolicyDiagnostic> GameServer::publishPermissions(
    shared::PolicyCompilation compilation,
    shared::PolicyCapabilityRegistry registry
)
{
    std::optional<shared::PolicyCapabilityKeyId> const flight_permission = capabilityKey(
        registry,
        "minecraft:flight"
    );
    std::optional<shared::PolicyCapabilityKeyId> const collision_bypass_permission = capabilityKey(
        registry,
        "minecraft:collision-bypass"
    );
    if (!flight_permission.has_value() || !collision_bypass_permission.has_value()) {
        return std::unexpected(policyDiagnostic(
            shared::PolicyDiagnosticCode::InvalidDeclaration,
            std::string{compilation.plan.sourceId()},
            "movement permission registry must define flight and collision bypass"
        ));
    }
    for (shared::PolicyCapabilityKeyId const key : {*flight_permission, *collision_bypass_permission}) {
        shared::PolicyCapabilityDefinition const& definition = registry.definitions[key];
        if (definition.minimum_value != 0 || definition.maximum_value != 1
            || definition.hard_restriction != shared::PolicyRestriction::Maximum) {
            return std::unexpected(policyDiagnostic(
                shared::PolicyDiagnosticCode::InvalidDeclaration,
                std::string{compilation.plan.sourceId()},
                "movement permissions must be boolean capabilities with maximum hard restrictions"
            ));
        }
    }

    std::vector<shared::PolicySubject> const subjects = permissionSubjects();
    auto capabilities = m_permission_host.materialize(compilation.plan, subjects, registry);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    if (!canApplyPublishedPermissions(*capabilities, registry)) {
        return std::unexpected(policyDiagnostic(
            shared::PolicyDiagnosticCode::InvalidDeclaration,
            std::string{compilation.plan.sourceId()},
            "movement policy is invalid for an active player or would strand a player in solid geometry"
        ));
    }

    auto const published = m_permission_host.publish(std::move(compilation), std::move(*capabilities));
    if (!published) {
        return std::unexpected(published.error());
    }
    m_permission_registry = std::move(registry);
    m_flight_permission = flight_permission;
    m_collision_bypass_permission = collision_bypass_permission;
    m_permissions_published = true;
    applyPublishedPermissions();
    return *published;
}

void GameServer::run()
{
    std::atomic_bool const never_stop{ false };
    run(never_stop);
}

void GameServer::run(std::atomic_bool const& stop_requested)
{
    run(stop_requested, nullptr);
}

void GameServer::run(
    std::atomic_bool const& stop_requested,
    BenchmarkHooks const* const benchmark_hooks
)
{
    if (!isValid()) {
        throw std::runtime_error{"Cannot run the game server without a valid listening socket"};
    }
    m_benchmark_metrics_enabled = benchmark_hooks != nullptr && static_cast<bool>(benchmark_hooks->on_preview_metrics);
    m_streaming_metrics = {};
    if (m_height_tile_workers != nullptr) {
        m_height_tile_workers->setBenchmarkMetricsEnabled(m_benchmark_metrics_enabled);
        m_height_tile_workers->setBeforeHeightTileCompletionHook(
            benchmark_hooks == nullptr ? std::function<void(core::ClientId, shared::HeightTileKey)>{}
                : benchmark_hooks->before_height_tile_completion
        );
    }
    bool const measure_network_events = benchmark_hooks != nullptr
        && static_cast<bool>(benchmark_hooks->on_tick);
    uint64_t network_events_since_tick = 0U;
    auto next_simulation = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point previous_loop_started_at{};
    while (!stop_requested.load(std::memory_order_relaxed)) {
        auto const now = std::chrono::steady_clock::now();
        auto const loop_started_at = m_benchmark_metrics_enabled
            ? now : std::chrono::steady_clock::time_point{};
        auto const loop_interval = m_benchmark_metrics_enabled && previous_loop_started_at.time_since_epoch().count() != 0
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(loop_started_at - previous_loop_started_at)
            : std::chrono::nanoseconds::zero();
        if (m_benchmark_metrics_enabled) {
            previous_loop_started_at = loop_started_at;
        }
        std::chrono::nanoseconds tick_duration{};
        std::chrono::nanoseconds network_poll_duration{};
        if (now >= next_simulation) {
            bool const measure_tick = benchmark_hooks != nullptr
                && (m_benchmark_metrics_enabled || static_cast<bool>(benchmark_hooks->on_tick));
            auto const tick_started_at = measure_tick
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            uint64_t const events = tickImpl(std::chrono::milliseconds::zero(), &stop_requested);
            if (measure_tick) {
                tick_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tick_started_at
                );
                if (benchmark_hooks != nullptr && benchmark_hooks->on_tick) {
                    benchmark_hooks->on_tick(tick_duration, network_events_since_tick + events);
                    network_events_since_tick = 0U;
                }
            }
            next_simulation += shared::TICK;
        } else {
            static constexpr std::chrono::milliseconds ACTIVE_STREAM_POLL_INTERVAL{1};
            static constexpr std::chrono::milliseconds IDLE_POLL_INTERVAL{5};
            bool const has_active_stream_work = std::ranges::any_of(
                m_preview_streams,
                [](PreviewStream const& stream) {
                    return !stream.queued_keys.empty()
                        || !stream.dispatched_keys.empty()
                        || !stream.ready_tiles.empty()
                        || !stream.pending_removals.empty()
                        || !stream.inflight_deliveries.empty();
                }
            ) || !m_pending_terrain_joins.empty()
                || std::ranges::any_of(m_player_replications, &PlayerReplication::hasAuthoritativeTerrainWork);
            std::chrono::milliseconds const poll_interval = has_active_stream_work
                ? ACTIVE_STREAM_POLL_INTERVAL
                : IDLE_POLL_INTERVAL;
            std::chrono::milliseconds const until_tick = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_simulation - std::chrono::steady_clock::now()
            );
            std::chrono::milliseconds const poll_timeout = std::max(
                std::chrono::milliseconds::zero(),
                std::min(poll_interval, until_tick)
            );
            auto const poll_started_at = m_benchmark_metrics_enabled
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            uint64_t const events = pollNetworkBatch(poll_timeout, &stop_requested);
            if (measure_network_events) {
                network_events_since_tick += events;
            }
            if (m_benchmark_metrics_enabled) {
                network_poll_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - poll_started_at
                );
            }
            processHeightTileStreams();
        }
        if (benchmark_hooks && benchmark_hooks->on_preview_buffered) {
            for (PreviewStream& stream : m_preview_streams) {
                benchmark_hooks->on_preview_buffered(
                    stream.client_id,
                    static_cast<uint32_t>(stream.ready_tiles.size() + stream.dispatched_keys.size())
                );
            }
        }
        if (benchmark_hooks && benchmark_hooks->on_preview_metrics) {
            HeightTileWorkerPool::QueueMetrics worker_queue_metrics{};
            if (m_height_tile_workers != nullptr) {
                worker_queue_metrics = m_height_tile_workers->queueMetrics();
            }
            auto const metrics_at = std::chrono::steady_clock::now();
            std::chrono::nanoseconds const loop_work = std::chrono::duration_cast<std::chrono::nanoseconds>(
                metrics_at - loop_started_at
            ) - network_poll_duration;
            for (PreviewStream& stream : m_preview_streams) {
                PreviewStreamMetrics metrics{
                    .height_tile_jobs = worker_queue_metrics.height_tiles,
                    .world_generation_jobs = worker_queue_metrics.world_generation,
                    .coverage_refresh = m_streaming_metrics.coverage_refresh,
                    .worker_result_publication = m_streaming_metrics.worker_result_publication,
                    .queue_refill = m_streaming_metrics.queue_refill,
                    .delivery = m_streaming_metrics.delivery,
                    .worker_dispatch = m_streaming_metrics.worker_dispatch,
                    .materialization_candidate_keys_examined =
                        m_streaming_metrics.materialization_candidate_keys_examined,
                    .materialization_resident_chunk_lookups =
                        m_streaming_metrics.materialization_resident_chunk_lookups,
                    .materialization_admission_attempts =
                        m_streaming_metrics.materialization_admission_attempts,
                    .materialization_admissions = m_streaming_metrics.materialization_admissions,
                    .materialization_duplicate_requests =
                        m_streaming_metrics.materialization_duplicate_requests,
                    .chunk_delivery_candidate_keys_examined =
                        m_streaming_metrics.chunk_delivery_candidate_keys_examined,
                    .chunk_delivery_resident_chunk_lookups =
                        m_streaming_metrics.chunk_delivery_resident_chunk_lookups,
                    .chunk_delivery_batches_sent = m_streaming_metrics.chunk_delivery_batches_sent,
                    .chunk_delivery_chunks_sent = m_streaming_metrics.chunk_delivery_chunks_sent,
                    .chunk_delivery_credits_consumed = m_streaming_metrics.chunk_delivery_credits_consumed,
                    .world_generation_jobs_dispatched = m_streaming_metrics.world_generation_jobs_dispatched,
                    .height_tile_jobs_dispatched = m_streaming_metrics.height_tile_jobs_dispatched,
                    .server_world_resident_chunks = m_physics_world == nullptr
                        ? 0U : m_physics_world->residentChunkCount(),
                    .queued_tiles = static_cast<uint32_t>(stream.queued_keys.size()),
                    .priority_scan_cursor = stream.priority_cursor,
                    .background_scan_cursor = stream.background_cursor,
                    .background_generation_eligible = metrics_at >= stream.background_generation_eligible_at,
                    .dispatched_tiles = static_cast<uint32_t>(stream.dispatched_keys.size()),
                    .pending_worker_jobs = worker_queue_metrics.height_tiles.pending_jobs
                        + worker_queue_metrics.world_generation.pending_jobs,
                    .submitted_worker_jobs = worker_queue_metrics.height_tiles.executor_queued_jobs
                        + worker_queue_metrics.height_tiles.running_jobs
                        + worker_queue_metrics.height_tiles.completed_uncollected_jobs
                        + worker_queue_metrics.world_generation.executor_queued_jobs
                        + worker_queue_metrics.world_generation.running_jobs
                        + worker_queue_metrics.world_generation.completed_uncollected_jobs,
                    .ready_tiles = static_cast<uint32_t>(stream.ready_tiles.size()),
                    .inflight_deliveries = static_cast<uint32_t>(stream.inflight_deliveries.size()),
                    .inflight_additions = static_cast<uint32_t>(stream.inflight_addition_keys.size()),
                    .inflight_removals = static_cast<uint32_t>(stream.inflight_removal_keys.size()),
                    .pending_removals = static_cast<uint32_t>(stream.pending_removals.size()),
                    .delivery_credits = stream.delivery_credits,
                    .resident_tiles = static_cast<uint32_t>(stream.resident_keys.size()),
                    .admitted_additions = stream.admitted_additions_since_metrics,
                    .rejected_delivery_acknowledgements = stream.rejected_delivery_acknowledgements,
                    .delivery_credit_samples = stream.delivery_credit_samples,
                    .delivery_credit_total = stream.delivery_credit_total,
                    .delivery_credit_max = stream.delivery_credit_max,
                    .server_loop_interval = loop_interval,
                    .server_loop_work = loop_work,
                    .server_tick = tick_duration,
                    .stream_pump = m_last_stream_pump_duration,
                    .network_poll = network_poll_duration,
                };
                populateNearWindowMetrics(stream, metrics);
                auto const replication = std::ranges::find(
                    m_player_replications,
                    stream.client_id,
                    &PlayerReplication::id
                );
                if (replication != m_player_replications.end()) {
                    metrics.latest_received_input_sequence = replication->latest_received_sequence;
                    metrics.acknowledged_input_sequence = replication->acknowledged_input_sequence;
                    metrics.unacknowledged_input_count = replication->latest_received_sequence
                        - replication->acknowledged_input_sequence;
                    metrics.pending_input_count = static_cast<uint32_t>(replication->pending_inputs.size());
                    metrics.authoritative_generation_cursor = replication->next_authoritative_generation_index;
                    metrics.authoritative_delivery_cursor = replication->next_authoritative_delivery_index;
                    metrics.authoritative_pending_jobs = static_cast<uint32_t>(replication->authoritative_jobs.size());
                    metrics.authoritative_inflight_batches = static_cast<uint32_t>(
                        replication->authoritative_deliveries.size()
                    );
                    metrics.acknowledged_authoritative_columns = static_cast<uint32_t>(
                        replication->acknowledged_authoritative_columns.size()
                    );
                    metrics.regional_artifact_bytes = m_world_generation.regionalArtifactBytes();
                    metrics.materialization_admission_failures = replication->materialization_admission_failures;
                    metrics.forecast_materialization_candidates_examined =
                        replication->forecast_materialization_candidates_since_metrics;
                    metrics.required_near_chunk_keys = static_cast<uint32_t>(
                        replication->required_near_chunks.size()
                    );
                    metrics.required_full_chunk_keys = static_cast<uint32_t>(
                        replication->required_chunks.size()
                    );
                    metrics.acknowledged_chunk_keys = static_cast<uint32_t>(std::ranges::count_if(
                        replication->acknowledged_chunks,
                        [replication](shared::ChunkCoordinate const coordinate) {
                            return replication->required_chunks.contains(coordinate);
                        }
                    ));
                    metrics.inflight_chunk_batches = static_cast<uint32_t>(
                        replication->inflight_chunk_deliveries.size()
                    );
                    for (auto const& [token, delivery] : replication->inflight_chunk_deliveries) {
                        static_cast<void>(token);
                        metrics.inflight_chunk_keys += static_cast<uint32_t>(delivery.coordinates.size());
                    }
                    metrics.chunk_delivery_credits = replication->chunk_delivery_credits;
                    metrics.near_required_chunks_all_resident =
                        replication->benchmark_near_chunks_all_resident;
                    replication->forecast_materialization_candidates_since_metrics = 0U;
                }
                benchmark_hooks->on_preview_metrics(stream.client_id, metrics);
                stream.admitted_additions_since_metrics = 0U;
            }
            m_streaming_metrics = {};
        }
    }
}

std::chrono::milliseconds GameServer::fixedTickDelay(std::chrono::milliseconds const elapsed) noexcept
{
    return elapsed < shared::TICK ? shared::TICK - elapsed : std::chrono::milliseconds::zero();
}

uint32_t GameServer::terrainWorkerCount(uint32_t const hardware_concurrency) noexcept
{
    static constexpr uint32_t RESERVED_SERVER_THREADS{ 1U };
    static constexpr uint32_t MAXIMUM_TERRAIN_WORKERS{ 8U };
    uint32_t const available = hardware_concurrency > RESERVED_SERVER_THREADS
        ? hardware_concurrency - RESERVED_SERVER_THREADS
        : 1U;
    return std::min(available, MAXIMUM_TERRAIN_WORKERS);
}

uint64_t GameServer::tick(std::chrono::milliseconds const timeout) {
    return tickImpl(timeout, nullptr);
}

uint64_t GameServer::pollNetworkBatch(
    std::chrono::milliseconds const timeout,
    std::atomic_bool const* const stop_requested
)
{
    return detail::pollNetworkEventBatch(
        [this](std::chrono::milliseconds const poll_timeout) {
            return poll(poll_timeout);
        },
        timeout,
        [stop_requested] {
            return stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed);
        }
    );
}

uint64_t GameServer::tickImpl(
    std::chrono::milliseconds const timeout,
    std::atomic_bool const* const stop_requested
)
{
    for (PlayerReplication& replication : m_player_replications) {
        replication.action_consumed_this_tick = false;
    }
    uint64_t const events = pollNetworkBatch(timeout, stop_requested);
    for (PlayerReplication& replication : m_player_replications) {
        if (!replication.action_consumed_this_tick && !replication.pending_inputs.empty()) {
            shared::ClientInputMessage const input = replication.pending_inputs.front();
            replication.pending_inputs.pop_front();
            if (!processInput(replication, input)) {
                replication.pending_inputs.push_front(input);
            }
        }
        if (!replication.action_consumed_this_tick && replication.pending_inputs.empty()
            && (!replication.terrain_session || replication.world_started)) {
            auto const player = m_world.player(replication.id);
            if (m_world.mode() == shared::WorldMode::Flight && player
                && !player->movement_capabilities.allows(shared::MovementCapability::Flight)) {
                static_cast<void>(processInput(replication, {
                    .direction = {}, .sequence = replication.acknowledged_input_sequence,
                }, false));
            }
        }
    }
    if (m_world.mode() == shared::WorldMode::Flight) {
        for (PlayerReplication const& replication : m_player_replications) {
            auto const player = m_world.player(replication.id);
            if (!player.has_value()) {
                continue;
            }
            shared::ChunkCoordinate const coordinate{
                .x = player->x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                .y = player->y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                .z = player->z / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            };
            if (m_physics_world != nullptr && shared::WorldBounds::isValidChunk(coordinate)
                && m_physics_world->residentChunk(coordinate) == nullptr) {
                static_cast<void>(m_world_generation.requestPlan(coordinate, m_generation_plan));
            }
        }
    }
    processHeightTileStreams();
    return events;
}

void GameServer::publishWorldPreparation(
    PendingTerrainJoin& join,
    generation::GenerationProgress const* const progress,
    std::shared_ptr<generation::GenerationSnapshot const> snapshot,
    bool const final_atlas_complete,
    std::optional<shared::PlayerPosition> const safe_spawn,
    std::string failure
)
{
    if (client(join.client_id) == std::nullopt) {
        return;
    }
    generation::GenerationStage stage = generation::GenerationStage::Tectonics;
    uint32_t work_completed = 0U;
    uint32_t work_total = 0U;
    if (progress != nullptr) {
        stage = progress->stage;
        work_completed = progress->completed;
        work_total = progress->total;
    }
    if (final_atlas_complete) {
        stage = generation::GenerationStage::Complete;
        if (progress == nullptr || progress->stage != generation::GenerationStage::Complete) {
            work_completed = 1U;
            work_total = 1U;
        }
    }
    if (!failure.empty()) {
        stage = generation::GenerationStage::Complete;
        work_completed = 0U;
        work_total = 0U;
        failure = boundedFailure(std::move(failure));
    }
    if (!final_atlas_complete && failure.empty() && stage == generation::GenerationStage::Complete) {
        return;
    }
    auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        WorldPreparation::Clock::now() - m_world_preparation->latest().request_accepted_at
    );
    sendTo(join.client_id, shared::ServerWorldPreparationMessage{
        .session = join.session,
        .sequence = join.next_message_sequence++,
        .stage = stage,
        .work_completed = work_completed,
        .work_total = work_total,
        .server_elapsed_microseconds = static_cast<uint64_t>(std::max<int64_t>(0, elapsed.count())),
        .latest_snapshot = std::move(snapshot),
        .final_atlas_complete = final_atlas_complete,
        .refined_safe_spawn = safe_spawn,
        .failure = std::move(failure),
    });
}

void GameServer::initializePreparedWorld(std::shared_ptr<shared::TerrainWorld const> terrain_world)
{
    if (m_prepared_world_initialized) {
        return;
    }
    if (terrain_world == nullptr || m_world_preparation == nullptr) {
        throw std::invalid_argument{ "prepared server runtime requires one ready canonical terrain world" };
    }
    terrain_world = checkedTerrainWorld(m_world.configuration(), std::move(terrain_world));
    shared::TerrainSessionIdentity const expected = terrainSessionIdentity(
        1U,
        m_world.configuration(),
        m_world_preparation->configuration()
    );
    if (terrain_world->identity() != expected.terrain) {
        throw std::invalid_argument{ "prepared terrain identity does not match the accepted world configuration" };
    }
    std::optional<shared::WorldGenerationPlan> const accepted_plan = m_world_preparation->generationPlan();
    if (!accepted_plan.has_value()
        || !hasCompleteTypedTerrainPlan(*accepted_plan)
        || std::ranges::find(accepted_plan->chunk_stages, shared::GenerationStage::Materialize)
            == accepted_plan->chunk_stages.end()) {
        throw std::invalid_argument{ "prepared terrain plan is absent or lacks the complete regional materialization path" };
    }
    m_terrain_world = std::move(terrain_world);
    m_generation_plan = *accepted_plan;
    m_world_generation.setTerrainContext(m_terrain_world, SERVER_TERRAIN_WORLD_EPOCH);
    shared::SparseWorldOptions options{
        .seed = m_world.configuration().seed,
        .max_resident_chunks = shared::MAX_FOUR_PLAYER_MATERIALIZED_INTEREST_CHUNK_KEYS,
        .revision = m_world.configuration().algorithm_version,
    };
    m_physics_world = std::make_unique<shared::SparseWorld>(options, m_terrain_world);
    m_world.setCollisionWorld(m_physics_world.get());
    m_height_tile_workers = std::make_unique<HeightTileWorkerPool>(
        m_terrain_world,
        terrainWorkerCount(std::thread::hardware_concurrency()),
        SERVER_TERRAIN_WORLD_EPOCH
    );
    m_height_tile_workers->setBenchmarkMetricsEnabled(m_benchmark_metrics_enabled);
    m_prepared_world_initialized = true;
    CORE_INFO(
        "Prepared Terrain-v2 server world: seed={}, revision={}, protected resident capacity={} chunks",
        m_terrain_world->identity().seed,
        m_terrain_world->identity().revision,
        m_physics_world->maxResidentChunks()
    );
}

std::expected<std::optional<shared::PlayerPosition>, std::string> GameServer::safeSpawnFor(
    PendingTerrainJoin& join
)
{
    if (m_physics_world == nullptr || m_terrain_world == nullptr) {
        return std::unexpected("canonical terrain residency is not initialized");
    }
    auto const explicit_spawn = std::ranges::find(m_spawn_points, join.character, &SpawnPoint::character);
    bool const has_explicit_spawn = explicit_spawn != m_spawn_points.end();
    shared::PlayerPosition initial = has_explicit_spawn
        ? shared::PlayerPosition{.x = explicit_spawn->x, .y = explicit_spawn->y, .z = explicit_spawn->z}
        : shared::PlayerPosition{
            .x = shared::World::FLIGHT_SPAWN.x,
            .y = shared::World::FLIGHT_SPAWN.y,
            .z = shared::World::FLIGHT_SPAWN.z,
        };

    static constexpr int64_t SUBCELL_SCALE = shared::SUBCELLS_PER_CELL;
    static constexpr int64_t PLAYER_WIDTH = shared::World::PLAYER_WIDTH_SUBCELLS;
    static constexpr int64_t PLAYER_HEIGHT = shared::World::PLAYER_HEIGHT_SUBCELLS;
    static constexpr int64_t WORLD_PERIOD = static_cast<int64_t>(shared::WorldExtent::WIDTH) * SUBCELL_SCALE;

    auto overlaps_player = [&initial](shared::Player const& player) {
        auto const horizontal_overlap = [](int64_t const first, int64_t const second) {
            int64_t delta = std::abs(first - second);
            delta = std::min(delta, WORLD_PERIOD - delta);
            return delta < PLAYER_WIDTH;
        };
        int64_t const candidate_x = static_cast<int64_t>(initial.x) * SUBCELL_SCALE + initial.x_subcell;
        int64_t const candidate_y = static_cast<int64_t>(initial.y) * SUBCELL_SCALE + initial.y_subcell;
        int64_t const candidate_z = static_cast<int64_t>(initial.z) * SUBCELL_SCALE + initial.z_subcell;
        int64_t const player_x = static_cast<int64_t>(player.x) * SUBCELL_SCALE + player.x_subcell;
        int64_t const player_y = static_cast<int64_t>(player.y) * SUBCELL_SCALE + player.y_subcell;
        int64_t const player_z = static_cast<int64_t>(player.z) * SUBCELL_SCALE + player.z_subcell;
        bool const vertical_overlap = candidate_z < player_z + PLAYER_HEIGHT
            && player_z < candidate_z + PLAYER_HEIGHT;
        return horizontal_overlap(candidate_x, player_x)
            && horizontal_overlap(candidate_y, player_y)
            && vertical_overlap;
    };

    uint32_t const candidate_count = has_explicit_spawn ? 1U : 64U;
    while (has_explicit_spawn || join.next_default_spawn_candidate < candidate_count) {
        if (!has_explicit_spawn) {
            initial.x = static_cast<int32_t>(shared::WorldBounds::wrapHorizontal(
                static_cast<int64_t>(shared::World::FLIGHT_SPAWN.x)
                + static_cast<int64_t>(join.next_default_spawn_candidate) * 2
            ));
            shared::HeightTileKey const key{
                .x = initial.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
                .y = initial.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH),
            };
            if (!join.surface_tile.has_value() || join.surface_key != key) {
                join.requested_surface_key = key;
                shared::WorldGenerationPlan surface_plan = m_generation_plan;
                surface_plan.chunk_stages = {shared::GenerationStage::HeightTile};
                shared::GenerationAdmission const admission = requestPendingJoinPlan(
                    join,
                    {.x = key.x, .y = key.y, .z = 0},
                    std::move(surface_plan)
                );
                if (admission == shared::GenerationAdmission::InvalidPlan
                    || admission == shared::GenerationAdmission::CapacityExceeded) {
                    return std::unexpected("canonical spawn height tile could not be admitted");
                }
                return std::optional<shared::PlayerPosition>{};
            }
            if (join.surface_tile->identity != m_terrain_world->identity()
                || join.surface_tile->coordinate != shared::ChunkCoordinate{.x = key.x, .y = key.y, .z = 0}
                || !shared::isValidSurfaceTile(*join.surface_tile)) {
                return std::unexpected("canonical spawn height tile has a mismatched terrain identity");
            }
            uint32_t const local_x = static_cast<uint32_t>(initial.x) % shared::Chunk::SIDE_LENGTH;
            uint32_t const local_y = static_cast<uint32_t>(initial.y) % shared::Chunk::SIDE_LENGTH;
            shared::TerrainColumn const& column = join.surface_tile->columns[
                local_y * shared::Chunk::SIDE_LENGTH + local_x
            ];
            initial.z = static_cast<int32_t>(column.highest_solid_z) + 1;
        }
        if (!shared::World::isFlightPositionInBounds(initial)) {
            if (has_explicit_spawn) {
                return std::unexpected("explicit spawn position is outside the canonical flight world");
            }
            ++join.next_default_spawn_candidate;
            continue;
        }

        int64_t const x_subcells = static_cast<int64_t>(initial.x) * SUBCELL_SCALE + initial.x_subcell;
        int64_t const y_subcells = static_cast<int64_t>(initial.y) * SUBCELL_SCALE + initial.y_subcell;
        int64_t const z_subcells = static_cast<int64_t>(initial.z) * SUBCELL_SCALE + initial.z_subcell;
        int64_t const first_x = x_subcells / SUBCELL_SCALE;
        int64_t const last_x = (x_subcells + PLAYER_WIDTH - 1) / SUBCELL_SCALE;
        int64_t const first_y = y_subcells / SUBCELL_SCALE;
        int64_t const last_y = (y_subcells + PLAYER_WIDTH - 1) / SUBCELL_SCALE;
        int64_t const first_z = z_subcells / SUBCELL_SCALE;
        int64_t const last_z = (z_subcells + PLAYER_HEIGHT - 1) / SUBCELL_SCALE;
        int64_t const first_checked_z = has_explicit_spawn ? first_z : first_z - 1;
        bool valid_vertical_bounds = true;
        std::vector<shared::ChunkCoordinate> missing_chunks;
        for (int64_t block_z = first_checked_z; block_z <= last_z && valid_vertical_bounds; ++block_z) {
            if (block_z >= static_cast<int64_t>(shared::WorldExtent::DEPTH)) {
                continue;
            }
            if (!shared::WorldBounds::isValidZ(block_z)) {
                valid_vertical_bounds = false;
                break;
            }
            for (int64_t block_y = first_y; block_y <= last_y; ++block_y) {
                for (int64_t block_x = first_x; block_x <= last_x; ++block_x) {
                    shared::WorldCoordinate const coordinate{
                        .x = static_cast<int64_t>(shared::WorldBounds::wrapHorizontal(block_x)),
                        .y = static_cast<int64_t>(shared::WorldBounds::wrapHorizontal(block_y)),
                        .z = block_z,
                    };
                    if (m_physics_world->queryBlock(coordinate).state == shared::GenerationState::Materialized) {
                        continue;
                    }
                    shared::ChunkCoordinate const chunk = shared::WorldBounds::chunkCoordinate(coordinate);
                    if (std::ranges::find(missing_chunks, chunk) == missing_chunks.end()) {
                        missing_chunks.push_back(chunk);
                    }
                }
            }
        }
        if (!valid_vertical_bounds) {
            if (has_explicit_spawn) {
                return std::unexpected("explicit spawn body is outside the canonical flight world");
            }
            ++join.next_default_spawn_candidate;
            continue;
        }
        if (!missing_chunks.empty()) {
            for (shared::ChunkCoordinate const coordinate : missing_chunks) {
                shared::GenerationAdmission const admission = requestPendingJoinPlan(
                    join,
                    coordinate,
                    m_generation_plan
                );
                if (admission == shared::GenerationAdmission::InvalidPlan
                    || admission == shared::GenerationAdmission::CapacityExceeded) {
                    return std::unexpected("canonical spawn collision chunks could not be admitted");
                }
            }
            return std::optional<shared::PlayerPosition>{};
        }

        bool body_clear = true;
        bool has_support = false;
        for (int64_t block_z = first_checked_z; block_z <= last_z && body_clear; ++block_z) {
            if (block_z >= static_cast<int64_t>(shared::WorldExtent::DEPTH)) {
                continue;
            }
            for (int64_t block_y = first_y; block_y <= last_y && body_clear; ++block_y) {
                for (int64_t block_x = first_x; block_x <= last_x; ++block_x) {
                    shared::WorldCoordinate const coordinate{
                        .x = static_cast<int64_t>(shared::WorldBounds::wrapHorizontal(block_x)),
                        .y = static_cast<int64_t>(shared::WorldBounds::wrapHorizontal(block_y)),
                        .z = block_z,
                    };
                    shared::SparseBlockQuery const query = m_physics_world->queryBlock(coordinate);
                    if (!has_explicit_spawn && block_z == first_z - 1) {
                        bool const materialized = query.state == shared::GenerationState::Materialized;
                        has_support = has_support || (materialized && query.block != shared::Block::Air);
                    } else if (block_z >= first_z
                        && (query.state != shared::GenerationState::Materialized
                            || query.block != shared::Block::Air)) {
                        body_clear = false;
                        break;
                    }
                }
            }
        }
        if (!body_clear || (!has_explicit_spawn && !has_support)) {
            if (has_explicit_spawn) {
                return std::unexpected("explicit spawn body intersects terrain or has no materialized support");
            }
            ++join.next_default_spawn_candidate;
            continue;
        }
        bool const overlaps_existing = std::ranges::any_of(m_world.players(), overlaps_player);
        if (!overlaps_existing) {
            return std::optional<shared::PlayerPosition>{initial};
        }
        if (has_explicit_spawn) {
            return std::unexpected("explicit spawn position overlaps an active player");
        }
        ++join.next_default_spawn_candidate;
    }
    return std::unexpected("canonical terrain has no safe default spawn near the world center");
}

shared::GenerationAdmission GameServer::requestPendingJoinPlan(
    PendingTerrainJoin& join,
    shared::ChunkCoordinate const coordinate,
    shared::WorldGenerationPlan plan
)
{
    shared::GenerationAdmission const admission = m_world_generation.requestPlan(
        coordinate,
        std::move(plan)
    );
    if (admission == shared::GenerationAdmission::Accepted
        || admission == shared::GenerationAdmission::Duplicate) {
        if (std::ranges::find(join.generation_requests, coordinate) == join.generation_requests.end()) {
            join.generation_requests.push_back(coordinate);
        }
    }
    return admission;
}

void GameServer::cancelPendingTerrainJoinWork(PendingTerrainJoin const& join) noexcept
{
    for (shared::ChunkCoordinate const coordinate : join.generation_requests) {
        bool const used_by_another_join = std::ranges::any_of(
            m_pending_terrain_joins,
            [&join, coordinate](PendingTerrainJoin const& candidate) {
                return &candidate != &join
                    && std::ranges::find(candidate.generation_requests, coordinate)
                        != candidate.generation_requests.end();
            }
        );
        bool const used_by_active_player = std::ranges::any_of(
            m_player_replications,
            [coordinate](PlayerReplication const& replication) {
                return replication.required_interest_chunks.contains(coordinate)
                    || replication.scheduled_chunks.contains(coordinate);
            }
        );
        bool const used_by_preview_stream = std::ranges::any_of(
            m_preview_streams,
            [coordinate](PreviewStream const& stream) {
                return stream.desires({.x = coordinate.x, .y = coordinate.y});
            }
        );
        if (!used_by_another_join && !used_by_active_player && !used_by_preview_stream) {
            static_cast<void>(m_world_generation.cancel(
                coordinate,
                m_world.configuration().algorithm_version,
                m_world.configuration().seed
            ));
        }
    }
}

void GameServer::activatePreparedJoin(PendingTerrainJoin join, shared::PlayerPosition const spawn)
{
    core::ClientId const id = join.client_id;
    if (client(id) == std::nullopt || m_world.player(id) || m_world.playerExists(join.character)) {
        return;
    }
    m_world.spawnPlayer(id, join.character, spawn, shared::defaultPlayerPaletteIndex(join.character));
    m_player_replications.push_back(PlayerReplication{
        .id = id,
        .pending_inputs = {},
        .last_accepted_direction = std::nullopt,
        .terrain_session = join.session,
        .wants_previews = join.wants_previews,
        .ordered_chunk_direction = std::nullopt,
        .ordered_chunk_keys = {},
        .ordered_prefetch_chunk_keys = {},
        .required_near_chunks = {},
        .required_chunks = {},
        .required_interest_chunks = {},
        .forecast_lead_chunks = {},
        .ordered_authoritative_columns = {},
        .required_authoritative_columns = {},
        .authoritative_interest_columns = {},
        .acknowledged_authoritative_columns = {},
        .inflight_authoritative_columns = {},
        .scheduled_authoritative_columns = {},
        .authoritative_jobs = {},
        .authoritative_deliveries = {},
        .authoritative_delivery_frontier = {},
        .acknowledged_chunks = {},
        .ready_delivery_frontier_keys = {},
        .current_delivery_frontier = {},
        .current_delivery_queue_keys = {},
        .near_delivery_frontier = {},
        .near_delivery_queue_keys = {},
        .prefetch_delivery_frontier = {},
        .prefetch_delivery_queue_keys = {},
        .waiting_delivery_frontier = {},
        .inflight_delivery_keys = {},
        .scheduled_chunks = {},
        .inflight_chunk_deliveries = {},
    });
    if (m_permissions_published) {
        auto const refreshed = refreshPublishedPermissions();
        if (!refreshed) {
            CORE_ERROR("Prepared player {} could not be admitted under published permissions: {}", id,
                refreshed.error().message);
            m_world.despawnPlayer(id);
            std::erase_if(m_player_replications, [id](PlayerReplication const& replication) {
                return replication.id == id;
            });
            sendTo(id, shared::ServerStartWorldAckMessage{
                .session = join.session,
                .accepted = false,
                .failure = boundedFailure(refreshed.error().message),
            });
            return;
        }
    }
    auto const spawned_player = m_world.player(id);
    if (!spawned_player) {
        return;
    }
    CORE_INFO("Player '{}' spawned at canonical safe position x {}, y {}, z {}", join.character,
        spawned_player->x, spawned_player->y, spawned_player->z);
    resetChunkCoverage(*playerReplication(id), spawn, true);
    for (shared::Player const& player : m_world.players()) {
        PlayerReplication* const replication = playerReplication(player.id);
        if (replication == nullptr) {
            continue;
        }
        shared::ServerPlayerPositionMessage const position = playerPositionMessage(player, *replication);
        if (player.id == id) {
            send(position);
        } else {
            sendTo(id, position);
        }
    }
    if (join.wants_previews) {
        startHeightTileStream(id);
    }
}

void GameServer::processWorldPreparation()
{
    if (m_world_preparation == nullptr) {
        return;
    }
    WorldPreparationSnapshot const snapshot = m_world_preparation->latest();
    if (snapshot.state == WorldPreparationState::Ready && !m_prepared_world_initialized) {
        try {
            initializePreparedWorld(snapshot.ready_world);
        } catch (std::exception const& error) {
            CORE_ERROR("Could not initialize the prepared Terrain-v2 server world: {}", error.what());
            while (!m_pending_terrain_joins.empty()) {
                auto join = std::make_unique<PendingTerrainJoin>(std::move(m_pending_terrain_joins.front()));
                m_pending_terrain_joins.erase(m_pending_terrain_joins.begin());
                publishWorldPreparation(*join, nullptr, snapshot.latest_map_snapshot, false, std::nullopt,
                    std::string{ "Terrain-v2 server initialization failed: " } + error.what());
                cancelPendingTerrainJoinWork(*join);
            }
            return;
        }
    }

    for (auto join = m_pending_terrain_joins.begin(); join != m_pending_terrain_joins.end();) {
        if (client(join->client_id) == std::nullopt) {
            cancelPendingTerrainJoinWork(*join);
            join = m_pending_terrain_joins.erase(join);
            continue;
        }
        if (snapshot.state == WorldPreparationState::Failed
            || snapshot.state == WorldPreparationState::Cancelled) {
            std::string failure = snapshot.error;
            if (failure.empty()) {
                failure = "Terrain-v2 world preparation was cancelled.";
            }
            publishWorldPreparation(*join, nullptr, snapshot.latest_map_snapshot, false, std::nullopt,
                std::move(failure));
            cancelPendingTerrainJoinWork(*join);
            join = m_pending_terrain_joins.erase(join);
            continue;
        }
        if (snapshot.state == WorldPreparationState::Ready) {
            auto spawn = safeSpawnFor(*join);
            if (!spawn) {
                publishWorldPreparation(*join, nullptr, snapshot.latest_map_snapshot, false, std::nullopt,
                    std::string{ "Terrain-v2 world has no safe spawn: " } + spawn.error());
                cancelPendingTerrainJoinWork(*join);
                join = m_pending_terrain_joins.erase(join);
                continue;
            }
            if (!spawn->has_value()) {
                ++join;
                continue;
            }
            generation::GenerationProgress const* progress = snapshot.latest_progress.has_value()
                ? &snapshot.latest_progress->progress : nullptr;
            shared::PlayerPosition const spawn_position = spawn.value().value();
            publishWorldPreparation(*join, progress, snapshot.latest_map_snapshot, true, spawn_position);
            auto ready = std::make_unique<PendingTerrainJoin>(std::move(*join));
            join = m_pending_terrain_joins.erase(join);
            activatePreparedJoin(std::move(*ready), spawn_position);
            continue;
        }
        if (snapshot.latest_progress.has_value()
            && snapshot.latest_progress->sequence > join->last_progress_source_sequence) {
            join->last_progress_source_sequence = snapshot.latest_progress->sequence;
            publishWorldPreparation(*join, &snapshot.latest_progress->progress, snapshot.latest_map_snapshot, false);
        } else if (join->next_message_sequence == 1U) {
            publishWorldPreparation(*join, nullptr, snapshot.latest_map_snapshot, false);
        }
        ++join;
    }
}

void GameServer::resetChunkCoverage(
    PlayerReplication& replication,
    shared::PlayerPosition const center,
    bool const force_new_epoch
)
{
    std::vector<shared::ChunkCoordinate> const near_keys = shared::requiredChunkKeys(
        center,
        NEAR_START_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const full_keys = shared::requiredChunkKeys(
        center,
        shared::NEAR_TERRAIN_RADIUS_BLOCKS
    );
    std::vector<shared::ChunkCoordinate> const interest_keys = shared::materializedInterestChunkKeys(center);
    PlayerReplication::ChunkKeySet near_set{ near_keys.begin(), near_keys.end() };
    PlayerReplication::ChunkKeySet full_set{ full_keys.begin(), full_keys.end() };
    PlayerReplication::ChunkKeySet interest_set{ interest_keys.begin(), interest_keys.end() };
    bool const changed_center = !replication.has_chunk_coverage
        || !samePosition(replication.coverage_center, center);
    bool const changed_full_keys = !replication.has_chunk_coverage
        || replication.required_chunks != full_set;
    bool const changed_interest_keys = !replication.has_chunk_coverage
        || replication.required_interest_chunks != interest_set;
    bool const changed_keys = !replication.has_chunk_coverage
        || replication.required_near_chunks != near_set || changed_full_keys || changed_interest_keys;
    std::optional<shared::Direction> const direction = materializedInterestDirection(replication);
    bool const changed_direction = !sameInterestDirection(replication.ordered_chunk_direction, direction);
    if (!changed_center && !changed_keys && !changed_direction && !force_new_epoch) {
        return;
    }

    bool new_epoch = force_new_epoch || !replication.has_chunk_coverage;
    if (!new_epoch && !chunkSetsTouch(replication.required_near_chunks, near_set)) {
        new_epoch = true;
    }
    std::vector<shared::ChunkCoordinate> newly_interested_keys;
    if (!new_epoch && changed_interest_keys) {
        for (shared::ChunkCoordinate const coordinate : interest_set) {
            if (!replication.required_interest_chunks.contains(coordinate)) {
                newly_interested_keys.push_back(coordinate);
            }
        }
    }
    if (new_epoch) {
        replication.coverage_epoch = replication.coverage_epoch == 0U
            ? 1U
            : (replication.coverage_epoch == std::numeric_limits<uint64_t>::max()
                ? 1U : replication.coverage_epoch + 1U);
        replication.chunk_credit_grant_received = false;
        replication.chunk_delivery_credits = 0U;
        replication.inflight_delivery_keys.clear();
        replication.acknowledged_chunks.clear();
        replication.ready_delivery_frontier_keys.clear();
        replication.current_delivery_frontier.clear();
        replication.current_delivery_queue_keys.clear();
        replication.near_delivery_frontier.clear();
        replication.near_delivery_queue_keys.clear();
        replication.prefetch_delivery_frontier.clear();
        replication.prefetch_delivery_queue_keys.clear();
        replication.waiting_delivery_frontier.clear();
    } else {
        PlayerReplication::ChunkKeySet const prior_acknowledged = replication.acknowledged_chunks;
        PlayerReplication::ChunkKeySet retained_acknowledged;
        retained_acknowledged.reserve(prior_acknowledged.size());
        for (shared::ChunkCoordinate const coordinate : prior_acknowledged) {
            if (interest_set.contains(coordinate)) {
                retained_acknowledged.insert(coordinate);
            }
        }
        PlayerReplication::ChunkKeySet continuity_seeds;
        continuity_seeds.reserve(std::min(replication.required_near_chunks.size(), near_set.size()));
        for (shared::ChunkCoordinate const coordinate : replication.required_near_chunks) {
            if (near_set.contains(coordinate) && retained_acknowledged.contains(coordinate)) {
                continuity_seeds.insert(coordinate);
            }
        }
        replication.acknowledged_chunks = rootedChunkComponent(retained_acknowledged, continuity_seeds);
    }
    bool const changed_profile_keys = !replication.has_chunk_coverage
        || center.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH)
            != replication.coverage_center.x / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH)
        || center.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH)
            != replication.coverage_center.y / static_cast<int32_t>(shared::Chunk::SIDE_LENGTH);
    if (new_epoch || changed_profile_keys) {
        auto const required_profiles = shared::requiredTerrainColumns(center);
        replication.required_authoritative_columns = PlayerReplication::ChunkKeySet{
            required_profiles.begin(), required_profiles.end()
        };
        auto profile_interest = shared::requiredTerrainColumns(center, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS);
        replication.authoritative_interest_columns = PlayerReplication::ChunkKeySet{
            profile_interest.begin(), profile_interest.end()
        };
        shared::ChunkCoordinate profile_center = shared::WorldBounds::chunkCoordinate({ center.x, center.y, 0 });
        auto const distance = [profile_center](shared::ChunkCoordinate const key) {
            auto const axis = [](int32_t const a, int32_t const b) {
                int32_t const delta = std::abs(a - b);
                return std::min(delta, 4'096 - delta);
            };
            return axis(key.x, profile_center.x) + axis(key.y, profile_center.y);
        };
        std::ranges::stable_sort(profile_interest, [&](auto const first, auto const second) {
            bool const first_required = replication.required_authoritative_columns.contains(first);
            bool const second_required = replication.required_authoritative_columns.contains(second);
            if (first_required != second_required) {
                return first_required;
            }
            return distance(first) < distance(second);
        });
        replication.ordered_authoritative_columns = std::move(profile_interest);
        auto const first_missing_profile = std::ranges::find_if(
            replication.ordered_authoritative_columns,
            [&](shared::ChunkCoordinate const key) {
                return !m_physics_world->hasAuthoritativeSurface(key)
                    && !replication.scheduled_authoritative_columns.contains(key);
            }
        );
        replication.next_authoritative_generation_index = static_cast<uint32_t>(
            first_missing_profile - replication.ordered_authoritative_columns.begin()
        );
        replication.next_authoritative_delivery_index = 0U;
    }
    if (new_epoch) {
        replication.acknowledged_authoritative_columns.clear();
        replication.inflight_authoritative_columns.clear();
    } else if (changed_profile_keys) {
        std::erase_if(replication.acknowledged_authoritative_columns, [&](auto const key) {
            return !replication.authoritative_interest_columns.contains(key);
        });
    }
    if (new_epoch || changed_profile_keys) {
        replication.authoritative_delivery_frontier.clear();
        for (uint32_t index = 0U; index < replication.ordered_authoritative_columns.size(); ++index) {
            shared::ChunkCoordinate const key = replication.ordered_authoritative_columns[index];
            if (!replication.acknowledged_authoritative_columns.contains(key)
                && !replication.inflight_authoritative_columns.contains(key)) {
                replication.authoritative_delivery_frontier.push_back(index);
            }
        }
    }
    replication.has_chunk_coverage = true;
    replication.coverage_center = center;
    replication.required_near_chunks = std::move(near_set);
    replication.required_chunks = std::move(full_set);
    replication.required_interest_chunks = std::move(interest_set);
    if (new_epoch || changed_keys || changed_direction) {
        rebuildMaterializedChunkOrders(replication, center, direction);
    }
    if (new_epoch) {
        replication.next_chunk_materialization_index = 0U;
        replication.next_prefetch_materialization_index = 0U;
        shared::ChunkCoordinate const root = shared::WorldBounds::chunkCoordinate({
            .x = static_cast<int64_t>(center.x),
            .y = static_cast<int64_t>(center.y),
            .z = static_cast<int64_t>(center.z),
        });
        enqueueMaterializedDeliveryCandidate(replication, root);
    } else if (changed_keys) {
        refreshMaterializedDeliveryFrontier(replication, false);
        PlayerReplication::ChunkKeySet const connected = connectedMaterializedDeliveryKeys(replication);
        shared::ChunkCoordinate const root = shared::WorldBounds::chunkCoordinate({
            .x = static_cast<int64_t>(center.x),
            .y = static_cast<int64_t>(center.y),
            .z = static_cast<int64_t>(center.z),
        });
        for (shared::ChunkCoordinate const coordinate : newly_interested_keys) {
            if (faceConnectedTo(coordinate, connected, root)) {
                enqueueMaterializedDeliveryCandidate(replication, coordinate);
            }
        }
    }
    if (changed_center || changed_keys || new_epoch) {
        ASSERT(replication.terrain_session.has_value(), "coverage publication requires an active terrain session");
        sendTo(replication.id, shared::ServerChunkCoverageMessage{
            .session = replication.terrain_session.value(),
            .coverage_epoch = replication.coverage_epoch,
            .center = center,
            .actual_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
            .visual_lod_radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        });
    }
    if (new_epoch || changed_interest_keys || changed_profile_keys) {
        updateProtectedChunkUnion();
    }
}

void GameServer::rebuildMaterializedChunkOrders(
    PlayerReplication& replication,
    shared::PlayerPosition const center,
    std::optional<shared::Direction> const direction
)
{
    std::optional<shared::ChunkCoordinate> prior_prefetch_cursor;
    if (!replication.ordered_prefetch_chunk_keys.empty()) {
        prior_prefetch_cursor = replication.ordered_prefetch_chunk_keys[
            replication.next_prefetch_materialization_index % replication.ordered_prefetch_chunk_keys.size()
        ];
    }
    replication.ordered_chunk_keys = connectedChunkOrder(
        center,
        replication.required_near_chunks,
        replication.required_chunks,
        direction
    );
    ChunkKeySet forecast_lead_keys;
    if (direction.has_value()) {
        shared::Direction const maximum_lead_direction{
            .x = direction->x,
            .y = direction->y,
            .z = direction->z,
            .accelerated = true,
            .speedup = 30U,
            .cycle_movement_capabilities = direction->cycle_movement_capabilities,
            .view_x = direction->view_x,
            .view_y = direction->view_y,
        };
        std::optional<shared::PlayerPosition> const forecast_center = m_world.forecastFlightPlayerPosition(
            replication.id,
            maximum_lead_direction
        );
        if (forecast_center.has_value()) {
            std::vector<shared::ChunkCoordinate> const forecast_keys = shared::requiredChunkKeys(
                *forecast_center,
                shared::NEAR_TERRAIN_RADIUS_BLOCKS
            );
            forecast_lead_keys.reserve(forecast_keys.size());
            for (shared::ChunkCoordinate const coordinate : forecast_keys) {
                if (replication.required_interest_chunks.contains(coordinate)
                    && !replication.required_chunks.contains(coordinate)) {
                    forecast_lead_keys.insert(coordinate);
                }
            }
        }
    }
    replication.ordered_prefetch_chunk_keys = connectedPrefetchOrder(
        center,
        replication.ordered_chunk_keys,
        replication.required_chunks,
        replication.required_interest_chunks,
        direction,
        forecast_lead_keys
    );
    replication.forecast_lead_chunks.clear();
    replication.forecast_lead_chunks.reserve(forecast_lead_keys.size());
    replication.forecast_lead_chunks.insert(forecast_lead_keys.begin(), forecast_lead_keys.end());
    auto const first_pending_forecast = std::ranges::find_if(
        replication.ordered_prefetch_chunk_keys,
        [this, &replication](shared::ChunkCoordinate const coordinate) {
            return replication.forecast_lead_chunks.contains(coordinate)
                && !replication.scheduled_chunks.contains(coordinate)
                && (m_physics_world == nullptr || m_physics_world->residentChunk(coordinate) == nullptr);
        }
    );
    if (first_pending_forecast != replication.ordered_prefetch_chunk_keys.end()) {
        replication.next_prefetch_materialization_index = static_cast<
            std::vector<shared::ChunkCoordinate>::size_type
        >(std::distance(replication.ordered_prefetch_chunk_keys.begin(), first_pending_forecast));
    } else if (prior_prefetch_cursor.has_value()) {
        auto const retained_cursor = std::ranges::find(
            replication.ordered_prefetch_chunk_keys,
            *prior_prefetch_cursor
        );
        if (retained_cursor != replication.ordered_prefetch_chunk_keys.end()) {
            replication.next_prefetch_materialization_index = static_cast<
                std::vector<shared::ChunkCoordinate>::size_type
            >(std::distance(replication.ordered_prefetch_chunk_keys.begin(), retained_cursor));
        } else if (!replication.ordered_prefetch_chunk_keys.empty()) {
            replication.next_prefetch_materialization_index %= replication.ordered_prefetch_chunk_keys.size();
        } else {
            replication.next_prefetch_materialization_index = 0U;
        }
    } else {
        replication.next_prefetch_materialization_index = 0U;
    }
    replication.ordered_chunk_direction = direction;
}

std::optional<shared::Direction> GameServer::materializedInterestDirection(
    PlayerReplication const& replication
) const noexcept
{
    if (!replication.pending_inputs.empty()) {
        return replication.pending_inputs.front().direction;
    }
    return replication.last_accepted_direction;
}

bool GameServer::nearChunksAreReady(PlayerReplication const& replication) const noexcept
{
    return replication.required_near_chunks.empty()
        || std::ranges::all_of(replication.required_near_chunks, [&replication](shared::ChunkCoordinate coordinate) {
            return replication.acknowledged_chunks.contains(coordinate);
        });
}

void GameServer::enqueueMaterializedDeliveryCandidate(
    PlayerReplication& replication,
    shared::ChunkCoordinate const coordinate
)
{
    if (m_physics_world == nullptr || !replication.required_interest_chunks.contains(coordinate)
        || replication.acknowledged_chunks.contains(coordinate)
        || replication.inflight_delivery_keys.contains(coordinate)
        || replication.ready_delivery_frontier_keys.contains(coordinate)
        || replication.waiting_delivery_frontier.contains(coordinate)) {
        return;
    }
    if (m_benchmark_metrics_enabled) {
        ++m_streaming_metrics.chunk_delivery_resident_chunk_lookups;
    }
    if (m_physics_world->residentChunk(coordinate) == nullptr) {
        replication.waiting_delivery_frontier.insert(coordinate);
        return;
    }
    replication.ready_delivery_frontier_keys.insert(coordinate);
    if (replication.required_near_chunks.contains(coordinate)) {
        if (replication.near_delivery_queue_keys.insert(coordinate).second) {
            replication.near_delivery_frontier.push_back(coordinate);
        }
    } else if (replication.required_chunks.contains(coordinate)) {
        if (replication.current_delivery_queue_keys.insert(coordinate).second) {
            replication.current_delivery_frontier.push_back(coordinate);
        }
    } else {
        if (replication.prefetch_delivery_queue_keys.insert(coordinate).second) {
            replication.prefetch_delivery_frontier.push_back(coordinate);
        }
    }
}

void GameServer::enqueueMaterializedDeliveryNeighbors(
    PlayerReplication& replication,
    shared::ChunkCoordinate const coordinate
)
{
    std::array<shared::ChunkCoordinate, 6U> neighbors = chunkNeighbors(coordinate);
    prioritizeNeighbors(
        neighbors,
        replication.coverage_center,
        materializedInterestDirection(replication)
    );
    for (shared::ChunkCoordinate const neighbor : neighbors) {
        enqueueMaterializedDeliveryCandidate(replication, neighbor);
    }
}

GameServer::PlayerReplication::ChunkKeySet GameServer::connectedMaterializedDeliveryKeys(
    PlayerReplication const& replication
) const
{
    PlayerReplication::ChunkKeySet connected = replication.acknowledged_chunks;
    PlayerReplication::ChunkKeySet inflight;
    inflight.reserve(replication.inflight_delivery_keys.size());
    for (shared::ChunkCoordinate const coordinate : replication.inflight_delivery_keys) {
        if (replication.required_interest_chunks.contains(coordinate)) {
            inflight.insert(coordinate);
        }
    }
    if (inflight.empty()) {
        return connected;
    }
    shared::ChunkCoordinate const root = shared::WorldBounds::chunkCoordinate({
        .x = static_cast<int64_t>(replication.coverage_center.x),
        .y = static_cast<int64_t>(replication.coverage_center.y),
        .z = static_cast<int64_t>(replication.coverage_center.z),
    });
    PlayerReplication::ChunkKeySet roots;
    roots.reserve(inflight.size());
    for (shared::ChunkCoordinate const coordinate : inflight) {
        if (faceConnectedTo(coordinate, connected, root)) {
            roots.insert(coordinate);
        }
    }
    PlayerReplication::ChunkKeySet const connected_inflight = rootedChunkComponent(inflight, roots);
    connected.insert(connected_inflight.begin(), connected_inflight.end());
    return connected;
}

void GameServer::refreshMaterializedDeliveryFrontier(
    PlayerReplication& replication,
    bool const seed_connected
)
{
    shared::ChunkCoordinate const root = shared::WorldBounds::chunkCoordinate({
        .x = static_cast<int64_t>(replication.coverage_center.x),
        .y = static_cast<int64_t>(replication.coverage_center.y),
        .z = static_cast<int64_t>(replication.coverage_center.z),
    });
    PlayerReplication::ChunkKeySet const connected = connectedMaterializedDeliveryKeys(replication);
    PlayerReplication::ChunkKeySet ready = std::move(replication.ready_delivery_frontier_keys);
    PlayerReplication::ChunkKeySet waiting = std::move(replication.waiting_delivery_frontier);
    replication.current_delivery_frontier.clear();
    replication.ready_delivery_frontier_keys.clear();
    replication.current_delivery_queue_keys.clear();
    replication.near_delivery_frontier.clear();
    replication.near_delivery_queue_keys.clear();
    replication.prefetch_delivery_frontier.clear();
    replication.prefetch_delivery_queue_keys.clear();
    replication.waiting_delivery_frontier.clear();

    auto const retain = [this, &replication, &connected, root](shared::ChunkCoordinate const coordinate) {
        if (!replication.required_interest_chunks.contains(coordinate)
            || replication.acknowledged_chunks.contains(coordinate)
            || replication.inflight_delivery_keys.contains(coordinate)
            || !faceConnectedTo(coordinate, connected, root)) {
            return;
        }
        enqueueMaterializedDeliveryCandidate(replication, coordinate);
    };
    for (shared::ChunkCoordinate const coordinate : ready) {
        retain(coordinate);
    }
    for (shared::ChunkCoordinate const coordinate : waiting) {
        retain(coordinate);
    }
    if (seed_connected) {
        for (shared::ChunkCoordinate const coordinate : connected) {
            enqueueMaterializedDeliveryNeighbors(replication, coordinate);
        }
    }
    if (connected.empty()) {
        enqueueMaterializedDeliveryCandidate(replication, root);
    }
}

void GameServer::refreshMaterializedCoverage()
{
    for (PlayerReplication& replication : m_player_replications) {
        if (!replication.terrain_session.has_value()) {
            continue;
        }
        auto const player = m_world.player(replication.id);
        if (!player.has_value()) {
            continue;
        }
        shared::PlayerPosition const center{
            .x = player->x,
            .y = player->y,
            .z = player->z,
            .x_subcell = player->x_subcell,
            .y_subcell = player->y_subcell,
            .z_subcell = player->z_subcell,
        };
        std::optional<shared::Direction> const direction = materializedInterestDirection(replication);
        if (!replication.has_chunk_coverage
            || !samePosition(replication.coverage_center, center)
            || !sameInterestDirection(replication.ordered_chunk_direction, direction)) {
            resetChunkCoverage(replication, center, false);
        }
    }
}

void GameServer::updateProtectedChunkUnion()
{
    if (m_physics_world == nullptr) {
        return;
    }
    PlayerReplication::ChunkKeySet protected_chunks;
    protected_chunks.reserve(shared::MAX_FOUR_PLAYER_MATERIALIZED_INTEREST_CHUNK_KEYS);
    for (PlayerReplication const& replication : m_player_replications) {
        if (!replication.terrain_session.has_value()) {
            continue;
        }
        protected_chunks.insert(
            replication.required_interest_chunks.begin(),
            replication.required_interest_chunks.end()
        );
    }
    PlayerReplication::ChunkKeySet protected_profiles;
    for (PlayerReplication const& replication : m_player_replications) {
        protected_profiles.insert(replication.authoritative_interest_columns.begin(),
            replication.authoritative_interest_columns.end());
    }
    std::vector<shared::ChunkCoordinate> profile_keys{ protected_profiles.begin(), protected_profiles.end() };
    if (!m_physics_world->setProtectedAuthoritativeColumns(profile_keys)) {
        CORE_ERROR("Could not protect authoritative terrain profile union ({} columns)", profile_keys.size());
    }
    std::vector<shared::ChunkCoordinate> coordinates;
    coordinates.reserve(protected_chunks.size());
    coordinates.insert(coordinates.end(), protected_chunks.begin(), protected_chunks.end());
    if (!m_physics_world->setProtectedChunks(coordinates)) {
        CORE_ERROR("Could not protect the exact active Terrain-v2 72-block materialized-interest union ({} keys)",
            protected_chunks.size());
    }
}

void GameServer::fillMaterializedChunkQueue()
{
    if (m_physics_world == nullptr || m_height_tile_workers == nullptr || m_terrain_world == nullptr) {
        return;
    }
    static constexpr uint32_t MAX_QUEUE_ATTEMPTS_PER_PLAYER = 256U;
    static constexpr uint32_t MAX_ADMISSIONS_PER_PLAYER = 48U;
    static constexpr uint32_t CURRENT_ADMISSION_SHARE = 36U;
    static constexpr uint32_t PREFETCH_ADMISSION_SHARE = 12U;
    static constexpr uint32_t CURRENT_ATTEMPT_SHARE = 192U;
    static constexpr uint32_t PREFETCH_ATTEMPT_SHARE = 64U;
    uint64_t const revision = m_terrain_world->identity().revision;
    uint64_t const seed = m_terrain_world->identity().seed;
    std::vector<PlayerReplication>::size_type const player_count = m_player_replications.size();
    if (player_count == 0U) {
        return;
    }
    std::vector<PlayerReplication>::size_type const first_player = m_next_materialization_admission % player_count;
    for (std::vector<PlayerReplication>::size_type offset = 0U; offset < player_count; ++offset) {
        std::vector<PlayerReplication>::size_type const player_index = (first_player + offset) % player_count;
        PlayerReplication& replication = m_player_replications[player_index];
        if (!replication.terrain_session.has_value() || replication.ordered_chunk_keys.empty()) {
            continue;
        }
        shared::PlayerPosition const center = replication.coverage_center;
        std::optional<shared::Direction> const direction = materializedInterestDirection(replication);
        if (!sameInterestDirection(replication.ordered_chunk_direction, direction)) {
            rebuildMaterializedChunkOrders(replication, center, direction);
        }
        uint32_t const near_count = static_cast<uint32_t>(replication.required_near_chunks.size());
        bool const near_materialized = std::ranges::all_of(
            replication.required_near_chunks,
            [this](shared::ChunkCoordinate const coordinate) {
                if (m_benchmark_metrics_enabled) {
                    ++m_streaming_metrics.materialization_resident_chunk_lookups;
                }
                return m_physics_world->residentChunk(coordinate) != nullptr;
            }
        );
        if (m_benchmark_metrics_enabled) {
            replication.benchmark_near_chunks_all_resident = near_materialized;
        }
        uint32_t total_attempts = 0U;
        uint32_t total_admissions = 0U;
        auto const enqueue_from = [this, &replication, revision, seed, near_count, near_materialized,
            &total_attempts, &total_admissions](
            std::vector<shared::ChunkCoordinate> const& order,
            PlayerReplication::ChunkKeySet const& allowed,
            std::vector<shared::ChunkCoordinate>::size_type& cursor,
            std::vector<shared::ChunkCoordinate>::size_type const order_limit,
            uint32_t attempt_limit,
            uint32_t admission_limit,
            bool const prefetch
        ) {
            uint32_t attempts = 0U;
            uint32_t admissions = 0U;
            bool queue_full = false;
            while (attempts < attempt_limit && admissions < admission_limit
                && total_attempts < MAX_QUEUE_ATTEMPTS_PER_PLAYER
                && total_admissions < MAX_ADMISSIONS_PER_PLAYER
                && order_limit > 0U && order_limit <= order.size()) {
                std::vector<shared::ChunkCoordinate>::size_type const index = cursor % order_limit;
                cursor = (index + 1U) % order_limit;
                ++attempts;
                ++total_attempts;
                if (m_benchmark_metrics_enabled) {
                    ++m_streaming_metrics.materialization_candidate_keys_examined;
                    ++m_streaming_metrics.materialization_resident_chunk_lookups;
                }
                shared::ChunkCoordinate const coordinate = order[index];
                if (m_benchmark_metrics_enabled && replication.forecast_lead_chunks.contains(coordinate)) {
                    ++replication.forecast_materialization_candidates_since_metrics;
                }
                if (!allowed.contains(coordinate)) {
                    continue;
                }
                if (m_physics_world->residentChunk(coordinate) != nullptr) {
                    replication.scheduled_chunks.erase(coordinate);
                    continue;
                }
                uint32_t const candidate_index = static_cast<uint32_t>(index);
                uint32_t const distance = near_materialized
                    ? postNearMaterializationPriority(
                        prefetch ? candidate_index : candidate_index - near_count,
                        near_count,
                        prefetch
                    )
                    : candidate_index;
                shared::GenerationPriorityScores const scores{
                    .distance = distance,
                    .view = 0U,
                    .movement = 0U,
                };
                shared::GenerationState const state = m_world_generation.state(coordinate, revision, seed);
                if (state == shared::GenerationState::Queued || state == shared::GenerationState::Running) {
                    replication.scheduled_chunks.insert(coordinate);
                    static_cast<void>(m_world_generation.updatePriority(coordinate, revision, seed, scores));
                    if (m_benchmark_metrics_enabled) {
                        ++m_streaming_metrics.materialization_duplicate_requests;
                    }
                    continue;
                }
                if (state == shared::GenerationState::Failed) {
                    if (m_benchmark_metrics_enabled) {
                        ++m_streaming_metrics.materialization_admission_attempts;
                    }
                    if (m_world_generation.requestRetry(coordinate, revision, seed)) {
                        replication.scheduled_chunks.insert(coordinate);
                        ++admissions;
                        ++total_admissions;
                        if (m_benchmark_metrics_enabled) {
                            ++m_streaming_metrics.materialization_admissions;
                        }
                    }
                    continue;
                }
                if (m_benchmark_metrics_enabled) {
                    ++m_streaming_metrics.materialization_admission_attempts;
                }
                shared::GenerationAdmission const admission = m_world_generation.requestPlan(
                    coordinate,
                    m_generation_plan,
                    scores
                );
                if (admission == shared::GenerationAdmission::Accepted) {
                    replication.scheduled_chunks.insert(coordinate);
                    ++admissions;
                    ++total_admissions;
                    if (m_benchmark_metrics_enabled) {
                        ++m_streaming_metrics.materialization_admissions;
                    }
                } else if (admission == shared::GenerationAdmission::Duplicate) {
                    replication.scheduled_chunks.insert(coordinate);
                    static_cast<void>(m_world_generation.updatePriority(coordinate, revision, seed, scores));
                    if (m_benchmark_metrics_enabled) {
                        ++m_streaming_metrics.materialization_duplicate_requests;
                    }
                } else if (admission == shared::GenerationAdmission::QueueFull) {
                    queue_full = true;
                    break;
                } else {
                    CORE_ERROR("Rejected canonical materialization request for chunk ({}, {}, {})",
                        coordinate.x, coordinate.y, coordinate.z);
                }
            }
            return std::tuple{ attempts, admissions, queue_full };
        };
        if (!near_materialized) {
            static_cast<void>(enqueue_from(
                replication.ordered_chunk_keys,
                replication.required_near_chunks,
                replication.next_chunk_materialization_index,
                replication.required_near_chunks.size(),
                MAX_QUEUE_ATTEMPTS_PER_PLAYER,
                MAX_ADMISSIONS_PER_PLAYER,
                false
            ));
            continue;
        }
        auto const [current_attempts, current_admissions, current_queue_full] = enqueue_from(
            replication.ordered_chunk_keys,
            replication.required_chunks,
            replication.next_chunk_materialization_index,
            replication.ordered_chunk_keys.size(),
            CURRENT_ATTEMPT_SHARE,
            CURRENT_ADMISSION_SHARE,
            false
        );
        static_cast<void>(current_queue_full);
        auto const [prefetch_attempts, prefetch_admissions, prefetch_queue_full] = enqueue_from(
            replication.ordered_prefetch_chunk_keys,
            replication.required_interest_chunks,
            replication.next_prefetch_materialization_index,
            replication.ordered_prefetch_chunk_keys.size(),
            PREFETCH_ATTEMPT_SHARE,
            PREFETCH_ADMISSION_SHARE,
            true
        );
        static_cast<void>(prefetch_queue_full);
        uint32_t const unused_attempts = MAX_QUEUE_ATTEMPTS_PER_PLAYER
            - current_attempts - prefetch_attempts;
        uint32_t const unused_admissions = MAX_ADMISSIONS_PER_PLAYER
            - current_admissions - prefetch_admissions;
        if (unused_attempts == 0U || unused_admissions == 0U) {
            continue;
        }
        uint32_t const current_unused_quota = current_admissions == CURRENT_ADMISSION_SHARE
            ? CURRENT_ATTEMPT_SHARE - current_attempts : 0U;
        uint32_t const prefetch_unused_quota = prefetch_admissions == PREFETCH_ADMISSION_SHARE
            ? PREFETCH_ATTEMPT_SHARE - prefetch_attempts : 0U;
        if (prefetch_unused_quota > 0U) {
            static_cast<void>(enqueue_from(
                replication.ordered_chunk_keys,
                replication.required_chunks,
                replication.next_chunk_materialization_index,
                replication.ordered_chunk_keys.size(),
                std::min(unused_attempts, prefetch_unused_quota),
                unused_admissions,
                false
            ));
        } else if (current_unused_quota > 0U) {
            static_cast<void>(enqueue_from(
                replication.ordered_prefetch_chunk_keys,
                replication.required_interest_chunks,
                replication.next_prefetch_materialization_index,
                replication.ordered_prefetch_chunk_keys.size(),
                std::min(unused_attempts, current_unused_quota),
                unused_admissions,
                true
            ));
        }
    }
    m_next_materialization_admission = (first_player + 1U) % player_count;
}

void GameServer::pumpMaterializedChunkDeliveries()
{
    if (m_physics_world == nullptr || m_terrain_world == nullptr) {
        return;
    }
    static constexpr size_t BATCH_CAPACITY = shared::MATERIALIZED_CHUNK_BATCH_CAPACITY;
    static constexpr uint32_t MAX_CANDIDATE_KEYS_PER_PLAYER_PER_PUMP = 1'024U;
    static constexpr uint32_t CURRENT_CANDIDATE_SHARE = 768U;
    static constexpr uint32_t PREFETCH_CANDIDATE_SHARE = 256U;
    static constexpr size_t CURRENT_CHUNKS_PER_BATCH = BATCH_CAPACITY * 3U / 4U;
    for (PlayerReplication& replication : m_player_replications) {
        if (!replication.terrain_session.has_value() || !replication.chunk_credit_grant_received) {
            continue;
        }
        shared::PlayerPosition const center = replication.coverage_center;
        std::optional<shared::Direction> const direction = materializedInterestDirection(replication);
        if (!sameInterestDirection(replication.ordered_chunk_direction, direction)) {
            rebuildMaterializedChunkOrders(replication, center, direction);
        }
        uint32_t candidate_budget_remaining = MAX_CANDIDATE_KEYS_PER_PLAYER_PER_PUMP;
        uint32_t current_budget_remaining = CURRENT_CANDIDATE_SHARE;
        uint32_t prefetch_budget_remaining = PREFETCH_CANDIDATE_SHARE;
        if (replication.chunk_delivery_credits == 0U
            || replication.inflight_chunk_deliveries.size() >= shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW) {
            continue;
        }
        uint32_t const available_batch_count = std::min<uint32_t>(
            replication.chunk_delivery_credits,
            static_cast<uint32_t>(shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW)
                - static_cast<uint32_t>(replication.inflight_chunk_deliveries.size())
        );
        uint32_t const maximum_outbound_keys = available_batch_count * static_cast<uint32_t>(BATCH_CAPACITY);
        PlayerReplication::ChunkKeySet connected_keys = connectedMaterializedDeliveryKeys(replication);
        connected_keys.reserve(connected_keys.size() + maximum_outbound_keys);
        PlayerReplication::ChunkKeySet inflight_keys = replication.inflight_delivery_keys;
        inflight_keys.reserve(inflight_keys.size() + maximum_outbound_keys);
        uint32_t inflight_bytes = 0U;
        for (auto const& [token, delivery] : replication.inflight_chunk_deliveries) {
            static_cast<void>(token);
            inflight_bytes += delivery.encoded_bytes;
        }
        while (candidate_budget_remaining > 0U
            && replication.chunk_delivery_credits > 0U
            && replication.inflight_chunk_deliveries.size() < shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW) {
            bool const near_complete = nearChunksAreReady(replication);
            bool const near_submitted = std::ranges::all_of(
                replication.required_near_chunks,
                [&replication](shared::ChunkCoordinate const coordinate) {
                    return replication.acknowledged_chunks.contains(coordinate)
                        || replication.inflight_delivery_keys.contains(coordinate);
                }
            );
            uint32_t const byte_capacity = std::min(
                MAX_PREPARED_STREAM_INFLIGHT_BYTES - inflight_bytes,
                remainingTerrainDeliveryBytes(replication.id)
            );
            std::optional<shared::TerrainSessionIdentity> const& terrain_session = replication.terrain_session;
            if (!terrain_session.has_value()) {
                break;
            }
            shared::ServerMaterializedChunkBatchMessage batch{
                .session = terrain_session.value(),
                .coverage_epoch = replication.coverage_epoch,
                .chunks = {},
            };
            if (shared::encodedMaterializedChunkBatchSize(batch) >= byte_capacity) {
                break;
            }
            std::vector<shared::MaterializedChunkPayload>& payloads = batch.chunks;
            std::vector<shared::ChunkCoordinate> delivered_coordinates;
            payloads.reserve(BATCH_CAPACITY);
            delivered_coordinates.reserve(BATCH_CAPACITY);
            shared::ChunkCoordinate const root = shared::WorldBounds::chunkCoordinate({
                .x = static_cast<int64_t>(center.x),
                .y = static_cast<int64_t>(center.y),
                .z = static_cast<int64_t>(center.z),
            });
            uint32_t near_candidate_budget = candidate_budget_remaining;
            auto const collect = [this, &replication, &payloads, &delivered_coordinates, &connected_keys,
                &inflight_keys, root, &candidate_budget_remaining, &batch, byte_capacity](
                std::deque<shared::ChunkCoordinate>& frontier,
                PlayerReplication::ChunkKeySet& queue_keys,
                size_t const maximum_payloads,
                uint32_t& class_budget_remaining
            ) {
                uint32_t const budget = std::min(candidate_budget_remaining, class_budget_remaining);
                uint32_t inspected = 0U;
                while (payloads.size() < maximum_payloads && inspected < budget && !frontier.empty()) {
                    shared::ChunkCoordinate const coordinate = frontier.front();
                    frontier.pop_front();
                    queue_keys.erase(coordinate);
                    ++inspected;
                    --candidate_budget_remaining;
                    --class_budget_remaining;
                    if (m_benchmark_metrics_enabled) {
                        ++m_streaming_metrics.chunk_delivery_candidate_keys_examined;
                    }
                    if (!replication.ready_delivery_frontier_keys.erase(coordinate)) {
                        continue;
                    }
                    if (!replication.required_interest_chunks.contains(coordinate)
                        || replication.acknowledged_chunks.contains(coordinate)
                        || inflight_keys.contains(coordinate)) {
                        replication.waiting_delivery_frontier.erase(coordinate);
                        continue;
                    }
                    if (!faceConnectedTo(coordinate, connected_keys, root)) {
                        replication.waiting_delivery_frontier.erase(coordinate);
                        continue;
                    }
                    if (m_benchmark_metrics_enabled) {
                        ++m_streaming_metrics.chunk_delivery_resident_chunk_lookups;
                    }
                    shared::Chunk const* const chunk = m_physics_world->residentChunk(coordinate);
                    if (chunk == nullptr) {
                        replication.waiting_delivery_frontier.insert(coordinate);
                        continue;
                    }
                    payloads.push_back(materializedChunkPayload(*chunk));
                    if (shared::encodedMaterializedChunkBatchSize(batch) > byte_capacity) {
                        payloads.pop_back();
                        replication.ready_delivery_frontier_keys.insert(coordinate);
                        frontier.push_front(coordinate);
                        queue_keys.insert(coordinate);
                        break;
                    }
                    delivered_coordinates.push_back(coordinate);
                    connected_keys.insert(coordinate);
                    inflight_keys.insert(coordinate);
                    replication.inflight_delivery_keys.insert(coordinate);
                    enqueueMaterializedDeliveryNeighbors(replication, coordinate);
                }
            };
            if (!near_complete && !near_submitted) {
                collect(
                    replication.near_delivery_frontier,
                    replication.near_delivery_queue_keys,
                    BATCH_CAPACITY,
                    near_candidate_budget
                );
            } else if (!near_complete) {
                collect(
                    replication.current_delivery_frontier,
                    replication.current_delivery_queue_keys,
                    BATCH_CAPACITY,
                    current_budget_remaining
                );
            } else {
                collect(
                    replication.current_delivery_frontier,
                    replication.current_delivery_queue_keys,
                    CURRENT_CHUNKS_PER_BATCH,
                    current_budget_remaining
                );
                collect(
                    replication.prefetch_delivery_frontier,
                    replication.prefetch_delivery_queue_keys,
                    BATCH_CAPACITY,
                    prefetch_budget_remaining
                );
                collect(
                    replication.current_delivery_frontier,
                    replication.current_delivery_queue_keys,
                    BATCH_CAPACITY,
                    current_budget_remaining
                );
                collect(
                    replication.prefetch_delivery_frontier,
                    replication.prefetch_delivery_queue_keys,
                    BATCH_CAPACITY,
                    prefetch_budget_remaining
                );
            }
            if (payloads.empty()) {
                break;
            }
            uint64_t token = replication.next_chunk_delivery_token++;
            if (token == 0U) {
                token = replication.next_chunk_delivery_token++;
            }
            uint32_t const encoded_bytes = static_cast<uint32_t>(shared::encodedMaterializedChunkBatchSize(batch));
            replication.inflight_chunk_deliveries.emplace(token, PlayerReplication::TerrainDelivery{
                .coordinates = std::move(delivered_coordinates), .encoded_bytes = encoded_bytes,
                .coverage_epoch = replication.coverage_epoch,
            });
            inflight_bytes += encoded_bytes;
            --replication.chunk_delivery_credits;
            if (m_benchmark_metrics_enabled) {
                ++m_streaming_metrics.chunk_delivery_batches_sent;
                m_streaming_metrics.chunk_delivery_chunks_sent += static_cast<uint64_t>(payloads.size());
                ++m_streaming_metrics.chunk_delivery_credits_consumed;
            }
            batch.delivery_token = token;
            sendTo(replication.id, std::move(batch));
        }
    }
}

void GameServer::acknowledgeMaterializedChunkDelivery(
    core::ClientId const client_id,
    shared::ClientChunkDeliveryAckMessage const& acknowledgement
)
{
    PlayerReplication* const replication = playerReplication(client_id);
    if (replication == nullptr || !replication->terrain_session.has_value()
        || *replication->terrain_session != acknowledgement.session) {
        return;
    }
    if (acknowledgement.delivery_token == 0U) {
        if (acknowledgement.credits != shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW
            || acknowledgement.coverage_epoch != replication->coverage_epoch
            || replication->chunk_credit_grant_received
            || !acknowledgement.not_resident_keys.empty()) {
            return;
        }
        replication->chunk_credit_grant_received = true;
        replication->chunk_delivery_credits = static_cast<uint8_t>(acknowledgement.credits
            - replication->inflight_chunk_deliveries.size());
        return;
    }
    if (acknowledgement.credits != 1U) {
        return;
    }
    auto const delivery = replication->inflight_chunk_deliveries.find(acknowledgement.delivery_token);
    if (delivery == replication->inflight_chunk_deliveries.end()
        || delivery->second.coverage_epoch != acknowledgement.coverage_epoch) {
        return;
    }
    for (shared::ChunkCoordinate const coordinate : acknowledgement.not_resident_keys) {
        if (std::ranges::find(delivery->second.coordinates, coordinate) == delivery->second.coordinates.end()) {
            return;
        }
    }
    if (acknowledgement.coverage_epoch != replication->coverage_epoch) {
        replication->inflight_chunk_deliveries.erase(delivery);
        if (replication->chunk_credit_grant_received) {
            replication->chunk_delivery_credits = std::min<uint8_t>(shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW,
                static_cast<uint8_t>(replication->chunk_delivery_credits + 1U));
        }
        return;
    }
    bool const has_nonresident_keys = !acknowledgement.not_resident_keys.empty();
    std::vector<shared::ChunkCoordinate> const delivered_coordinates = delivery->second.coordinates;
    for (shared::ChunkCoordinate const coordinate : delivered_coordinates) {
        replication->inflight_delivery_keys.erase(coordinate);
        bool const resident = std::ranges::find(acknowledgement.not_resident_keys, coordinate)
            == acknowledgement.not_resident_keys.end();
        if (resident && replication->required_interest_chunks.contains(coordinate)) {
            replication->acknowledged_chunks.insert(coordinate);
        } else {
            replication->acknowledged_chunks.erase(coordinate);
        }
    }
    replication->inflight_chunk_deliveries.erase(delivery);
    replication->chunk_delivery_credits = std::min<uint8_t>(
        shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW,
        static_cast<uint8_t>(replication->chunk_delivery_credits + 1U)
    );
    if (has_nonresident_keys) {
        PlayerReplication::ChunkKeySet roots;
        for (shared::ChunkCoordinate const coordinate : replication->required_near_chunks) {
            if (replication->acknowledged_chunks.contains(coordinate)) {
                roots.insert(coordinate);
            }
        }
        shared::ChunkCoordinate const root = shared::WorldBounds::chunkCoordinate({
            .x = static_cast<int64_t>(replication->coverage_center.x),
            .y = static_cast<int64_t>(replication->coverage_center.y),
            .z = static_cast<int64_t>(replication->coverage_center.z),
        });
        if (roots.empty() && replication->acknowledged_chunks.contains(root)) {
            roots.insert(root);
        }
        replication->acknowledged_chunks = rootedChunkComponent(replication->acknowledged_chunks, roots);
        refreshMaterializedDeliveryFrontier(*replication, true);
        PlayerReplication::ChunkKeySet const connected = connectedMaterializedDeliveryKeys(*replication);
        for (shared::ChunkCoordinate const coordinate : acknowledgement.not_resident_keys) {
            if (replication->required_interest_chunks.contains(coordinate)
                && faceConnectedTo(coordinate, connected, root)) {
                enqueueMaterializedDeliveryCandidate(*replication, coordinate);
            }
        }
    } else {
        for (shared::ChunkCoordinate const coordinate : delivered_coordinates) {
            if (replication->acknowledged_chunks.contains(coordinate)) {
                enqueueMaterializedDeliveryNeighbors(*replication, coordinate);
            }
        }
    }
}

void GameServer::startPreparedWorld(
    core::ClientId const client_id,
    shared::ClientStartWorldMessage const& start
)
{
    PlayerReplication* const replication = playerReplication(client_id);
    if (replication == nullptr || !replication->terrain_session.has_value()
        || *replication->terrain_session != start.session) {
        return;
    }
    if (!nearChunksAreReady(*replication)) {
        sendTo(client_id, shared::ServerStartWorldAckMessage{
            .session = start.session,
            .accepted = false,
            .failure = "The required 40-block terrain sphere is not resident yet.",
        });
        return;
    }
    replication->world_started = true;
    sendTo(client_id, shared::ServerStartWorldAckMessage{
        .session = start.session,
        .accepted = true,
        .failure = {},
    });
}

bool GameServer::rerootPlayerCoverage(
    shared::PlayerId const player_id,
    shared::PlayerPosition const position
)
{
    PlayerReplication* const replication = playerReplication(player_id);
    if (replication == nullptr || !replication->terrain_session.has_value()
        || m_physics_world == nullptr || !shared::World::isFlightPositionInBounds(position)) {
        return false;
    }
    int64_t const x_subcells = static_cast<int64_t>(position.x) * shared::SUBCELLS_PER_CELL
        + position.x_subcell;
    int64_t const y_subcells = static_cast<int64_t>(position.y) * shared::SUBCELLS_PER_CELL
        + position.y_subcell;
    int64_t const z_subcells = static_cast<int64_t>(position.z) * shared::SUBCELLS_PER_CELL
        + position.z_subcell;
    int64_t const last_x = (x_subcells + shared::World::PLAYER_WIDTH_SUBCELLS - 1)
        / shared::SUBCELLS_PER_CELL;
    int64_t const last_y = (y_subcells + shared::World::PLAYER_WIDTH_SUBCELLS - 1)
        / shared::SUBCELLS_PER_CELL;
    int64_t const last_z = (z_subcells + shared::World::PLAYER_HEIGHT_SUBCELLS - 1)
        / shared::SUBCELLS_PER_CELL;
    for (int64_t z = position.z; z <= last_z; ++z) {
        if (z >= static_cast<int64_t>(shared::WorldExtent::DEPTH)) {
            continue;
        }
        if (!shared::WorldBounds::isValidZ(z)) {
            return false;
        }
        for (int64_t y = position.y; y <= last_y; ++y) {
            for (int64_t x = position.x; x <= last_x; ++x) {
                shared::WorldCoordinate const coordinate{
                    .x = shared::WorldBounds::wrapHorizontal(x),
                    .y = shared::WorldBounds::wrapHorizontal(y),
                    .z = z,
                };
                // Rerooting may use resident collision data but cannot generate it synchronously.
                if (m_physics_world->queryBlock(coordinate).state != shared::GenerationState::Materialized) {
                    return false;
                }
            }
        }
    }
    if (!m_world.setPlayerPosition(player_id, position)) {
        return false;
    }
    ++replication->state_revision;
    auto const updated_player = m_world.player(player_id);
    if (!updated_player) {
        return false;
    }
    resetChunkCoverage(*replication, position, true);
    send(playerPositionMessage(*updated_player, *replication));
    flush();
    return true;
}

std::expected<std::vector<GameServer::SpawnPoint>, std::string> GameServer::validateSpawnPoints(
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode
) {
    for (SpawnPoint const& spawn_point : spawn_points) {
        bool const valid_character = spawn_point.character == '@'
            || spawn_point.character == '#'
            || spawn_point.character == '$'
            || spawn_point.character == '%'
            || spawn_point.character == '&';
        if (!valid_character) {
            return std::unexpected("spawn point has an unsupported character");
        }
        bool const outside_flat_world = world_mode == shared::WorldMode::Flat
            && (spawn_point.x < 0 || spawn_point.x > shared::World::MAX_PLAYER_ORIGIN_CELL
                || spawn_point.y < 0 || spawn_point.y > shared::World::MAX_PLAYER_ORIGIN_CELL
                || spawn_point.z != 0);
        bool const outside_flight_world = world_mode == shared::WorldMode::Flight
            && (spawn_point.z < shared::World::FLIGHT_MIN_CELL
                || spawn_point.z > shared::World::FLIGHT_ENTITY_MAX_Z);
        if (outside_flat_world || outside_flight_world) {
            return std::unexpected("spawn point is outside the world");
        }
    }
    for (auto first = spawn_points.begin(); first != spawn_points.end(); ++first) {
        for (auto second = std::next(first); second != spawn_points.end(); ++second) {
            if (first->character == second->character) {
                return std::unexpected("spawn points contain duplicate characters");
            }
            int32_t const horizontal_distance = std::abs(first->x - second->x);
            int32_t const vertical_distance = std::abs(first->y - second->y);
            if (world_mode == shared::WorldMode::Flat
                && horizontal_distance <= 1 && vertical_distance <= 1) {
                return std::unexpected("spawn points overlap player collision neighborhoods");
            }
        }
    }
    return spawn_points;
}

std::vector<GameServer::SpawnPoint> GameServer::checkedSpawnPoints(
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode
) {
    auto validated = validateSpawnPoints(
        std::move(spawn_points),
        world_mode
    );
    if (!validated.has_value()) {
        throw std::invalid_argument{ validated.error() };
    }
    return std::move(*validated);
}

void GameServer::onConnected(core::ServerConnectEvent const client) {
    CORE_INFO("Server: onConnected {}", client.client.address());
}

void GameServer::onDisconnected(core::ServerDisconnectEvent const client) {
    CORE_INFO("Server: onDisconnected {}", client.client.address());
    for (;;) {
        auto const join = std::ranges::find(m_pending_terrain_joins, client.client_id, &PendingTerrainJoin::client_id);
        if (join == m_pending_terrain_joins.end()) {
            break;
        }
        PendingTerrainJoin canceled = std::move(*join);
        m_pending_terrain_joins.erase(join);
        cancelPendingTerrainJoinWork(canceled);
    }
    std::erase_if(m_preview_streams, [this, &client](PreviewStream& stream) {
        if (stream.client_id != client.client_id) {
            return false;
        }
        if (m_height_tile_workers != nullptr) {
            m_height_tile_workers->cancel(stream.client_id, stream.generation);
        }
        stream.scheduler.invalidateRevision(stream.generation);
        return true;
    });
    auto const player = m_world.player(client.client_id);
    if (!player.has_value()) {
        return;
    }
    send(shared::ServerRemovePlayerMessage{
        .ch = player->ch,
    });
    m_world.despawnPlayer(client.client_id);
    std::erase_if(m_player_replications, [&client](PlayerReplication const& replication) {
        return replication.id == client.client_id;
    });
    updateProtectedChunkUnion();
    if (m_permissions_published) {
        auto const refreshed = refreshPublishedPermissions(
            static_cast<shared::PolicyEntityId>(client.client_id)
        );
        if (!refreshed) {
            CORE_ERROR("Failed to refresh permissions after player departure: {}", refreshed.error().message);
        }
    }
}

void GameServer::onReceived(core::ServerReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        CORE_ERROR("Received a corrupted message");
        return;
    }

    shared::PlayerId const id = event.client_id;
    shared::Message* msg_ptr = &*maybe_msg;
    if (auto* join_request = std::get_if<shared::JoinRequestMessage>(msg_ptr)) {
        auto const reject_join = [this, id, join_request] {
            sendTo(id, shared::JoinResponseMessage{
                .accepted = false,
                .join_epoch = join_request->join_epoch,
            });
        };
        if (join_request->wants_previews && m_terrain_world == nullptr && m_world_preparation == nullptr) {
            CORE_ERROR("Rejected preview join because the server has no prepared terrain context");
            reject_join();
            return;
        }
        if (join_request->mode != m_world.mode() || join_request->configuration != m_world.configuration()) {
            reject_join();
            return;
        }
        char const ch = join_request->ch;
        bool const pending_id = std::ranges::any_of(m_pending_terrain_joins, [id](PendingTerrainJoin const& join) {
            return join.client_id == id;
        });
        bool const pending_character = std::ranges::any_of(
            m_pending_terrain_joins,
            [ch](PendingTerrainJoin const& join) { return join.character == ch; }
        );
        if (m_world.player(id) || m_world.playerExists(ch) || pending_id || pending_character) {
            reject_join();
            return;
        }
        if (m_world_preparation != nullptr) {
            shared::TerrainSessionIdentity session;
            try {
                session = terrainSessionIdentity(
                    join_request->join_epoch,
                    m_world.configuration(),
                    m_world_preparation->configuration()
                );
            } catch (std::exception const& error) {
                CORE_ERROR("Rejected terrain join because its accepted identity is invalid: {}", error.what());
                reject_join();
                return;
            }
            m_pending_terrain_joins.push_back(PendingTerrainJoin{
                .client_id = id,
                .character = ch,
                .session = std::move(session),
                .wants_previews = join_request->wants_previews,
                .requested_surface_key = std::nullopt,
                .surface_key = std::nullopt,
                .surface_tile = std::nullopt,
                .generation_requests = {},
            });
            sendTo(id, shared::JoinResponseMessage{
                .accepted = true,
                .join_epoch = join_request->join_epoch,
            });
            return;
        }
        auto const spawn_point = std::ranges::find(
            m_spawn_points,
            ch,
            &SpawnPoint::character
        );
        if (spawn_point == m_spawn_points.end()) {
            m_world.spawnPlayer(id, ch, std::nullopt, shared::defaultPlayerPaletteIndex(ch));
        } else {
            m_world.spawnPlayer(id, ch, shared::PlayerPosition{
                .x = spawn_point->x,
                .y = spawn_point->y,
                .z = spawn_point->z,
            }, shared::defaultPlayerPaletteIndex(ch));
        }
        m_player_replications.push_back(PlayerReplication{
            .id = id,
            .pending_inputs = {},
            .last_accepted_direction = std::nullopt,
            .terrain_session = std::nullopt,
            .ordered_chunk_direction = std::nullopt,
            .ordered_chunk_keys = {},
            .ordered_prefetch_chunk_keys = {},
            .required_near_chunks = {},
            .required_chunks = {},
            .required_interest_chunks = {},
            .forecast_lead_chunks = {},
            .ordered_authoritative_columns = {},
            .required_authoritative_columns = {},
            .authoritative_interest_columns = {},
            .acknowledged_authoritative_columns = {},
            .inflight_authoritative_columns = {},
            .scheduled_authoritative_columns = {},
            .authoritative_jobs = {},
            .authoritative_deliveries = {},
            .authoritative_delivery_frontier = {},
            .acknowledged_chunks = {},
            .ready_delivery_frontier_keys = {},
            .current_delivery_frontier = {},
            .current_delivery_queue_keys = {},
            .near_delivery_frontier = {},
            .near_delivery_queue_keys = {},
            .prefetch_delivery_frontier = {},
            .prefetch_delivery_queue_keys = {},
            .waiting_delivery_frontier = {},
            .inflight_delivery_keys = {},
            .scheduled_chunks = {},
            .inflight_chunk_deliveries = {},
        });
        if (m_permissions_published) {
            auto const refreshed = refreshPublishedPermissions();
            if (!refreshed) {
                CORE_ERROR("Rejected player join because active permissions could not be materialized: {}",
                    refreshed.error().message);
                m_world.despawnPlayer(id);
                std::erase_if(m_player_replications, [id](PlayerReplication const& replication) {
                    return replication.id == id;
                });
                reject_join();
                return;
            }
        }
        auto const spawned_player = m_world.player(id);
        if (!spawned_player.has_value()) {
            CORE_ERROR("Spawned player {} is missing from the world", id);
            m_world.despawnPlayer(id);
            std::erase_if(m_player_replications, [id](PlayerReplication const& replication) {
                return replication.id == id;
            });
            reject_join();
            return;
        }
        sendTo(id, shared::JoinResponseMessage{
            .accepted = true,
            .join_epoch = join_request->join_epoch,
        });
        CORE_INFO("Player '{}' spawned at x {}, y {}, z {}", ch, spawned_player->x, spawned_player->y,
            spawned_player->z);
        for (shared::Player const& player : m_world.players()) {
            PlayerReplication* const replication = playerReplication(player.id);
            if (replication == nullptr) {
                continue;
            }
            shared::ServerPlayerPositionMessage const position = playerPositionMessage(player, *replication);
            if (player.id == id) {
                send(position);
            } else {
                sendTo(id, position);
            }
        }
        if (join_request->wants_previews) {
            startHeightTileStream(id);
        }
    } else if (auto* client_input = std::get_if<shared::ClientInputMessage>(msg_ptr)) {
        auto const player = m_world.player(id);
        PlayerReplication* const replication = playerReplication(id);
        if (!player || replication == nullptr) {
            return;
        }
        if (replication->terrain_session.has_value() && !replication->world_started) {
            return;
        }
        if (replication->has_received_sequence
            && !shared::isNewerSequence(client_input->sequence, replication->latest_received_sequence)) {
            return;
        }
        if ((replication->action_consumed_this_tick || !replication->pending_inputs.empty())
            && replication->pending_inputs.size() == PlayerReplication::MAX_PENDING_INPUTS) {
            return;
        }
        replication->latest_received_sequence = client_input->sequence;
        replication->has_received_sequence = true;
        if (!replication->action_consumed_this_tick && replication->pending_inputs.empty()) {
            if (!processInput(*replication, *client_input)) {
                replication->pending_inputs.push_back(*client_input);
            }
        } else {
            replication->pending_inputs.push_back(*client_input);
        }
    } else if (auto* authority_ack = std::get_if<shared::ClientAuthoritativeTerrainAckMessage>(msg_ptr)) {
        acknowledgeAuthoritativeTerrain(id, *authority_ack);
    } else if (auto* height_tile_credit = std::get_if<shared::ClientHeightTileCreditMessage>(msg_ptr)) {
        acknowledgeHeightTileDelivery(id, *height_tile_credit);
    } else if (auto* chunk_acknowledgement = std::get_if<shared::ClientChunkDeliveryAckMessage>(msg_ptr)) {
        acknowledgeMaterializedChunkDelivery(id, *chunk_acknowledgement);
    } else if (auto* world_start = std::get_if<shared::ClientStartWorldMessage>(msg_ptr)) {
        startPreparedWorld(id, *world_start);
    } else {
        CORE_ERROR("Received a message unsupported by the server {}", msg_ptr->index());
    }
}

bool GameServer::processInput(
    PlayerReplication& replication,
    shared::ClientInputMessage const input,
    bool const acknowledge_input
)
{
    auto const player_before_input = m_world.player(replication.id);
    if (!player_before_input.has_value()) {
        CORE_ERROR("Cannot process input for missing player {}", replication.id);
        return true;
    }
    bool capabilities_changed = false;
    if (input.direction.cycle_movement_capabilities) {
        uint8_t next_bits = player_before_input->movement_capabilities.bits == 3U
            ? 1U : (player_before_input->movement_capabilities.bits == 1U ? 0U : 3U);
        if (m_permissions_published) {
            std::shared_ptr<shared::PolicySnapshot const> const policy = m_permission_host.snapshot();
            std::shared_ptr<shared::PolicyCapabilitySnapshot const> const effective = policy
                ? policy->capabilities() : nullptr;
            if (effective == nullptr || !m_flight_permission.has_value()
                || !m_collision_bypass_permission.has_value()) {
                next_bits = 0U;
            } else {
                uint8_t allowed_bits = effective->allows({
                    shared::PolicyCapabilityQuery::SubjectId{replication.id},
                    shared::PolicyCapabilityQuery::CapabilityKeyId{*m_flight_permission},
                }) ? 1U : 0U;
                if ((allowed_bits & 1U) != 0U
                    && effective->allows({
                        shared::PolicyCapabilityQuery::SubjectId{replication.id},
                        shared::PolicyCapabilityQuery::CapabilityKeyId{*m_collision_bypass_permission},
                    })) {
                    allowed_bits |= 2U;
                }
                next_bits &= allowed_bits;
            }
        }
        capabilities_changed = m_world.setPlayerMovementCapabilities(
            replication.id,
            { .bits = next_bits }
        );
    }
    auto const player_before_movement = m_world.player(replication.id);
    if (!player_before_movement.has_value()) {
        CORE_ERROR("Cannot simulate input for missing player {}", replication.id);
        return true;
    }
    bool const should_simulate = input.direction.x != 0 || input.direction.y != 0 || input.direction.z != 0
        || !player_before_movement->movement_capabilities.allows(shared::MovementCapability::Flight);
    bool moved = false;
    bool generation_failed = false;
    if (should_simulate) {
        uint64_t const revision = m_world.configuration().algorithm_version;
        uint64_t const seed = m_world.configuration().seed;
        std::vector<shared::ChunkCoordinate> missing_chunks;
        for (shared::ChunkCoordinate const coordinate : m_world.flightCollisionChunks(
                 replication.id,
                 input.direction
             )) {
            if (m_physics_world->queryBlock({
                    .x = static_cast<int64_t>(coordinate.x) * shared::Chunk::SIDE_LENGTH,
                    .y = static_cast<int64_t>(coordinate.y) * shared::Chunk::SIDE_LENGTH,
                    .z = static_cast<int64_t>(coordinate.z) * shared::Chunk::SIDE_LENGTH,
                }).state == shared::GenerationState::Materialized) {
                continue;
            }
            shared::GenerationState const state = m_world_generation.state(coordinate, revision, seed);
            if (state == shared::GenerationState::Failed
                && !m_world_generation.requestRetry(coordinate, revision, seed)) {
                generation_failed = true;
                break;
            }
            if (state == shared::GenerationState::Cancelled) {
                generation_failed = true;
                break;
            }
            missing_chunks.push_back(coordinate);
        }
        bool const plan_materializes_chunks = std::ranges::find(
            m_generation_plan.chunk_stages,
            shared::GenerationStage::Materialize
        ) != m_generation_plan.chunk_stages.end();
        if (!missing_chunks.empty() && !plan_materializes_chunks) {
            generation_failed = true;
        }
        bool queue_full = false;
        if (!generation_failed) {
            for (shared::ChunkCoordinate const coordinate : missing_chunks) {
                shared::GenerationAdmission const admission = m_world_generation.requestPlan(
                    coordinate,
                    m_generation_plan,
                    generationPriorityScores(*player_before_movement, input.direction, coordinate)
                );
                if (admission == shared::GenerationAdmission::InvalidPlan) {
                    generation_failed = true;
                    break;
                }
                if (admission == shared::GenerationAdmission::QueueFull) {
                    queue_full = true;
                    if (++replication.materialization_admission_failures
                        >= PlayerReplication::MAX_MATERIALIZATION_ADMISSION_FAILURES) {
                        generation_failed = true;
                    }
                    break;
                }
            }
        }
        if (!queue_full) {
            replication.materialization_admission_failures = 0U;
        }
        if (!missing_chunks.empty() && !generation_failed) {
            if (capabilities_changed) {
                static_cast<void>(m_world.setPlayerMovementCapabilities(
                    replication.id,
                    player_before_input->movement_capabilities
                ));
            }
            return false;
        }
        if (!generation_failed) {
            moved = m_world.movePlayer(replication.id, input.direction);
        }
    }
    auto const player_after_movement = m_world.player(replication.id);
    if (!player_after_movement.has_value()) {
        CORE_ERROR("Cannot publish input result for missing player {}", replication.id);
        return true;
    }
    replication.materialization_admission_failures = 0U;
    replication.action_consumed_this_tick = true;
    bool const velocity_changed = player_after_movement->vertical_velocity_subcells
        != player_before_input->vertical_velocity_subcells;
    if (!acknowledge_input && !moved && !capabilities_changed && !velocity_changed) {
        return true;
    }
    if (acknowledge_input) {
        replication.acknowledged_input_sequence = input.sequence;
        replication.last_accepted_direction = input.direction;
    }
    auto const stream = std::ranges::find(m_preview_streams, replication.id, &PreviewStream::client_id);
    if (acknowledge_input && stream != m_preview_streams.end()) {
        stream->last_actual_direction = input.direction;
        stream->last_actual_direction->cycle_movement_capabilities = false;
        int8_t const movement_x = static_cast<int8_t>(input.direction.x);
        int8_t const movement_y = static_cast<int8_t>(input.direction.y);
        stream->heading_x = movement_x != 0 || movement_y != 0 ? movement_x : input.direction.view_x;
        stream->heading_y = movement_x != 0 || movement_y != 0 ? movement_y : input.direction.view_y;
    }
    if (replication.terrain_session.has_value()) {
        refreshMaterializedCoverage();
    }
    ++replication.state_revision;
    shared::ServerPlayerPositionMessage const position = playerPositionMessage(
        *player_after_movement,
        replication
    );
    if (moved || capabilities_changed || velocity_changed) {
        send(position);
    } else {
        sendTo(replication.id, position);
    }
    // Submit the latency-sensitive acknowledgement before this tick admits
    // additional reliable terrain fragments on the bulk channel.
    flush();
    return true;
}

GameServer::PlayerReplication* GameServer::playerReplication(shared::PlayerId const id) noexcept
{
    for (PlayerReplication& replication : m_player_replications) {
        if (replication.id == id) {
            return &replication;
        }
    }
    return nullptr;
}

shared::ServerPlayerPositionMessage GameServer::playerPositionMessage(
    shared::Player const& player,
    PlayerReplication const& replication
) const noexcept
{
    return {
        .ch = player.ch,
        .palette_index = player.palette_index,
        .movement_capabilities = player.movement_capabilities,
        .x = player.x,
        .y = player.y,
        .z = player.z,
        .x_subcell = player.x_subcell,
        .y_subcell = player.y_subcell,
        .z_subcell = player.z_subcell,
        .vertical_velocity_subcells = player.vertical_velocity_subcells,
        .acknowledged_input_sequence = replication.acknowledged_input_sequence,
        .state_revision = replication.state_revision,
    };
}

void GameServer::send(shared::Message message) {
    sendTo(std::nullopt, std::move(message));
}

uint32_t GameServer::remainingTerrainDeliveryBytes(core::ClientId const client_id) const noexcept
{
    uint64_t inflight_bytes = 0U;
    auto const replication = std::ranges::find(m_player_replications, client_id, &PlayerReplication::id);
    if (replication != m_player_replications.end()) {
        for (auto const& [token, delivery] : replication->inflight_chunk_deliveries) {
            static_cast<void>(token);
            inflight_bytes += delivery.encoded_bytes;
        }
        for (auto const& [token, delivery] : replication->authoritative_deliveries) {
            static_cast<void>(token);
            inflight_bytes += delivery.encoded_bytes;
        }
    }
    auto const preview = std::ranges::find(m_preview_streams, client_id, &PreviewStream::client_id);
    if (preview != m_preview_streams.end()) {
        for (auto const& [token, delivery] : preview->inflight_deliveries) {
            static_cast<void>(token);
            inflight_bytes += delivery.encoded_bytes;
        }
    }
    return inflight_bytes < MAX_TERRAIN_INFLIGHT_BYTES
        ? MAX_TERRAIN_INFLIGHT_BYTES - static_cast<uint32_t>(inflight_bytes) : 0U;
}

void GameServer::sendTo(std::optional<core::ClientId> const client_id, shared::Message message) {
    auto const* preparation = std::get_if<shared::ServerWorldPreparationMessage>(&message);
    bool const diagnostic_preparation = preparation != nullptr
        && (preparation->sequence <= 32U || preparation->final_atlas_complete);
    uint64_t const diagnostic_sequence = preparation == nullptr ? 0U : preparation->sequence;
    auto const encode_started_at = diagnostic_preparation
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    uint8_t const channel = std::holds_alternative<shared::ServerMaterializedChunkBatchMessage>(message)
        || std::holds_alternative<shared::ServerAuthoritativeTerrainBatchMessage>(message)
        ? shared::HEIGHT_TILE_CHANNEL : shared::GAME_CHANNEL;
    std::vector const message_bytes = shared::encodeMessage(std::move(message));
    auto const encode_completed_at = diagnostic_preparation
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::optional<core::Peer> peer;
    if (client_id.has_value()) {
        peer = client(*client_id);
        if (!peer) {
            CORE_ERROR("Cannot send a message to disconnected client {}", *client_id);
            return;
        }
    }
    auto const send_started_at = diagnostic_preparation
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    bool const sent = core::Server::send(peer, message_bytes, channel,
        core::SendMode{ core::SendMode::Reliable });
    auto const send_completed_at = diagnostic_preparation
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!sent) {
        CORE_ERROR("Failed to send a message");
    }
    if (diagnostic_preparation) {
        CORE_INFO("startup-boundary preparation-server sequence={} encode_steady_ns={} packet_bytes={} "
            "encode_us={} send_steady_ns={} send_us={} sent={}",
            diagnostic_sequence,
            std::chrono::duration_cast<std::chrono::nanoseconds>(encode_started_at.time_since_epoch()).count(),
            message_bytes.size(),
            std::chrono::duration_cast<std::chrono::microseconds>(encode_completed_at - encode_started_at).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(send_started_at.time_since_epoch()).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(send_completed_at - send_started_at).count(), sent);
    }
}

std::vector<shared::PolicySubject> GameServer::permissionSubjects() const
{
    std::vector<shared::PolicySubject> subjects;
    subjects.reserve(m_world.players().size());
    for (shared::Player const& player : m_world.players()) {
        subjects.push_back({
            .id = player.id,
            .entity_class = shared::PolicyEntityClass::Player,
            .kind = "player",
        });
    }
    return subjects;
}

bool GameServer::canApplyPublishedPermissions(
    shared::PolicyCapabilitySnapshot const& capabilities,
    shared::PolicyCapabilityRegistry const& registry
) const
{
    std::optional<shared::PolicyCapabilityKeyId> const flight_permission = capabilityKey(
        registry,
        "minecraft:flight"
    );
    std::optional<shared::PolicyCapabilityKeyId> const collision_bypass_permission = capabilityKey(
        registry,
        "minecraft:collision-bypass"
    );
    if (!flight_permission.has_value() || !collision_bypass_permission.has_value()) {
        return false;
    }
    for (shared::Player const& player : m_world.players()) {
        std::optional<int64_t> const flight = capabilities.value({
            shared::PolicyCapabilityQuery::SubjectId{player.id},
            shared::PolicyCapabilityQuery::CapabilityKeyId{*flight_permission},
        });
        std::optional<int64_t> const collision_bypass = capabilities.value({
            shared::PolicyCapabilityQuery::SubjectId{player.id},
            shared::PolicyCapabilityQuery::CapabilityKeyId{*collision_bypass_permission},
        });
        if (!flight.has_value() || !collision_bypass.has_value()
            || (*flight != 0 && *flight != 1)
            || (*collision_bypass != 0 && *collision_bypass != 1)) {
            return false;
        }
        shared::MovementCapabilities const movement{
            .bits = static_cast<uint8_t>((*flight != 0 ? 1U : 0U) | (*collision_bypass != 0 ? 2U : 0U)),
        };
        if (!shared::isValidMovementCapabilities(movement)
            || !m_world.canSetPlayerMovementCapabilities(player.id, movement)) {
            return false;
        }
    }
    return true;
}

std::expected<uint64_t, shared::PolicyDiagnostic> GameServer::refreshPublishedPermissions(
    std::optional<shared::PolicyEntityId> const expired_subject
)
{
    std::shared_ptr<shared::PolicySnapshot const> const active = m_permission_host.snapshot();
    if (!m_permissions_published || active == nullptr) {
        return 0U;
    }
    shared::PolicyCompilation compilation{
        .plan = active->plan(),
        .backend_receipt = active->backendReceipt(),
        .interpreter_parity = active->interpreterParity(),
    };
    if (expired_subject.has_value()) {
        compilation.plan = compilation.plan.withoutSubject(*expired_subject);
    }
    std::vector<shared::PolicySubject> const subjects = permissionSubjects();
    auto capabilities = m_permission_host.materialize(compilation.plan, subjects, m_permission_registry);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    if (!canApplyPublishedPermissions(*capabilities, m_permission_registry)) {
        return std::unexpected(policyDiagnostic(
            shared::PolicyDiagnosticCode::InvalidDeclaration,
            std::string{compilation.plan.sourceId()},
            "active movement policy is invalid for an active player or would strand a player in solid geometry"
        ));
    }
    auto const published = m_permission_host.publish(std::move(compilation), std::move(*capabilities));
    if (!published) {
        return std::unexpected(published.error());
    }
    applyPublishedPermissions();
    return *published;
}

void GameServer::applyPublishedPermissions()
{
    std::shared_ptr<shared::PolicySnapshot const> const active = m_permission_host.snapshot();
    auto const flight_permission = m_flight_permission;
    auto const collision_bypass_permission = m_collision_bypass_permission;
    if (active == nullptr || !flight_permission.has_value() || !collision_bypass_permission.has_value()) {
        return;
    }
    std::shared_ptr<shared::PolicyCapabilitySnapshot const> const capabilities = active->capabilities();
    if (capabilities == nullptr) {
        return;
    }
    for (shared::Player const& player : m_world.players()) {
        std::optional<int64_t> const flight = capabilities->value({
            shared::PolicyCapabilityQuery::SubjectId{player.id},
            shared::PolicyCapabilityQuery::CapabilityKeyId{*flight_permission},
        });
        std::optional<int64_t> const collision_bypass = capabilities->value(
            {
                shared::PolicyCapabilityQuery::SubjectId{player.id},
                shared::PolicyCapabilityQuery::CapabilityKeyId{*collision_bypass_permission},
            }
        );
        if (!flight.has_value() || !collision_bypass.has_value()) {
            continue;
        }
        shared::MovementCapabilities const movement{
            .bits = static_cast<uint8_t>((*flight != 0 ? 1U : 0U) | (*collision_bypass != 0 ? 2U : 0U)),
        };
        if (movement == player.movement_capabilities) {
            continue;
        }
        if (!m_world.setPlayerMovementCapabilities(player.id, movement)) {
            CORE_ERROR("Validated permission update could not be applied to player {}", player.id);
            continue;
        }
        PlayerReplication* const replication = playerReplication(player.id);
        if (replication != nullptr) {
            auto const updated_player = m_world.player(player.id);
            if (!updated_player.has_value()) {
                CORE_ERROR("Updated permission target {} is missing from the world", player.id);
                continue;
            }
            ++replication->state_revision;
            send(playerPositionMessage(*updated_player, *replication));
        }
    }
}

void GameServer::sendHeightTileTo(
    std::optional<core::ClientId> const client_id,
    shared::Message message
) {
    std::vector const message_bytes = shared::encodeMessage(std::move(message));
    std::optional<core::Peer> peer;
    if (client_id.has_value()) {
        peer = client(*client_id);
        if (!peer) {
            return;
        }
    }
    if (!core::Server::send(peer, message_bytes, shared::HEIGHT_TILE_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a height tile message");
    }
}

void GameServer::startHeightTileStream(core::ClientId const client_id)
{
    sendHeightTileTo(client_id, shared::ServerHeightTileDescriptorMessage{
        .configuration = m_world.configuration(),
        .world_identity = m_terrain_world->identity(),
        .world_revision = PreviewStream::WORLD_REVISION,
        .max_height_tiles = shared::heightTileInterestCount(m_height_tile_interest_orders->radius),
        .max_height_tile_bytes = shared::HEIGHT_TILE_PAYLOAD_BYTES,
    });
    sendHeightTileTo(client_id, shared::ServerWorldRevisionMessage{ .world_revision = PreviewStream::WORLD_REVISION });
    m_preview_streams.emplace_back(client_id, m_height_tile_interest_orders->radius);
}

void GameServer::acknowledgeHeightTileDelivery(
    core::ClientId const client_id,
    shared::ClientHeightTileCreditMessage const& credit
)
{
    if (credit.world_revision != PreviewStream::WORLD_REVISION) {
        return;
    }
    auto const stream = std::ranges::find(m_preview_streams, client_id, &PreviewStream::client_id);
    if (stream == m_preview_streams.end()) {
        return;
    }
    auto const reject_acknowledgement = [&stream] {
        ++stream->rejected_delivery_acknowledgements;
    };
    if (credit.delivery_token == 0U) {
        if (credit.credits != shared::HEIGHT_TILE_DELIVERY_WINDOW
            || !credit.not_resident_keys.empty()
            || stream->delivery_credits != 0U || !stream->inflight_deliveries.empty()) {
            reject_acknowledgement();
            return;
        }
        stream->delivery_credits = credit.credits;
        return;
    }
    if (credit.credits != 1U) {
        reject_acknowledgement();
        return;
    }
    auto const delivery = stream->inflight_deliveries.find(credit.delivery_token);
    if (delivery == stream->inflight_deliveries.end()) {
        reject_acknowledgement();
        return;
    }
    for (shared::HeightTileKey const key : credit.not_resident_keys) {
        bool const belongs_to_delivery = std::ranges::find(delivery->second.additions, key)
            != delivery->second.additions.end();
        if (!belongs_to_delivery && !stream->resident_keys.contains(key)) {
            reject_acknowledgement();
            return;
        }
    }
    if (delivery->second.admitted_at.time_since_epoch().count() != 0) {
        auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - delivery->second.admitted_at
        );
        ++stream->delivery_credit_samples;
        stream->delivery_credit_total += elapsed;
        stream->delivery_credit_max = std::max(stream->delivery_credit_max, elapsed);
    }
    for (shared::HeightTileKey const key : credit.not_resident_keys) {
        stream->resident_keys.erase(key);
        if (stream->desires(key)) {
            stream->priority_cursor = 0U;
        }
    }
    for (shared::HeightTileKey const key : delivery->second.additions) {
        stream->inflight_addition_keys.erase(key);
        bool const client_has_tile = std::ranges::find(credit.not_resident_keys, key)
            == credit.not_resident_keys.end();
        if (client_has_tile) {
            stream->resident_keys.insert(key);
        } else {
            stream->resident_keys.erase(key);
        }
        if (client_has_tile && !stream->desires(key) && !stream->inflight_removal_keys.contains(key)) {
            if (stream->pending_removals.insert(key).second) {
                stream->removal_candidates.clear();
            }
        }
    }
    for (shared::HeightTileKey const key : delivery->second.removals) {
        stream->inflight_removal_keys.erase(key);
        stream->pending_removals.erase(key);
        stream->resident_keys.erase(key);
        if (stream->desires(key)) {
            stream->priority_cursor = 0U;
        }
    }
    stream->inflight_deliveries.erase(delivery);
    if (stream->delivery_credits < shared::HEIGHT_TILE_DELIVERY_WINDOW) {
        ++stream->delivery_credits;
    }
}

uint32_t GameServer::admitHeightTileDeliveries(
    PreviewStream& stream,
    uint32_t const maximum_batches
)
{
    // Keep the existing terrain byte reserve, not a fixed number of raw packets,
    // below the shared 65,536-byte reliable window so gameplay retains headroom.
    uint32_t inflight_bytes = 0U;
    for (auto const& [token, delivery] : stream.inflight_deliveries) {
        static_cast<void>(token);
        inflight_bytes += delivery.encoded_bytes;
    }
    PlayerReplication const* const replication = playerReplication(stream.client_id);
    uint32_t const stream_byte_limit = replication != nullptr && replication->terrain_session
        ? MAX_PREPARED_STREAM_INFLIGHT_BYTES : PreviewStream::MAX_INFLIGHT_DELIVERY_BYTES;
    uint32_t const available_batches = std::min<uint32_t>({
        stream.delivery_credits,
        static_cast<uint32_t>(PreviewStream::MAX_INFLIGHT_DELIVERIES - stream.inflight_deliveries.size()),
        maximum_batches,
    });
    uint32_t const maximum_operations = available_batches * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    if (maximum_operations == 0U) {
        return 0U;
    }

    auto const removal_order = [](
        PreviewStream::RemovalCandidate const& first,
        PreviewStream::RemovalCandidate const& second
    ) {
        return std::tuple{ first.priority, first.key.y, first.key.x }
            < std::tuple{ second.priority, second.key.y, second.key.x };
    };
    if (stream.removal_candidates.empty() && !stream.pending_removals.empty()) {
        stream.removal_candidates.reserve(stream.pending_removals.size());
        for (shared::HeightTileKey const key : stream.pending_removals) {
            if (!stream.inflight_removal_keys.contains(key) && !stream.desires(key)) {
                stream.removal_candidates.push_back({
                    .key = key,
                    .priority = shared::heightTileInterestPriority(
                        stream.center, stream.heading_x, stream.heading_y, key
                    ),
                });
            }
        }
        std::ranges::make_heap(stream.removal_candidates, removal_order);
    }
    std::vector<shared::HeightTileKey> removal_keys;
    removal_keys.reserve(maximum_operations);
    while (removal_keys.size() < maximum_operations && !stream.removal_candidates.empty()) {
        std::ranges::pop_heap(stream.removal_candidates, removal_order);
        shared::HeightTileKey const key = stream.removal_candidates.back().key;
        stream.removal_candidates.pop_back();
        if (stream.pending_removals.contains(key) && !stream.inflight_removal_keys.contains(key)
            && !stream.desires(key)) {
            removal_keys.push_back(key);
        }
    }

    auto const is_available_for_far_addition = [&stream](shared::HeightTileKey const key) {
        return (stream.resident_keys.contains(key) && !stream.inflight_removal_keys.contains(key))
            || stream.inflight_addition_keys.contains(key);
    };
    bool const current_near_has_pending_additions = nearWindowHasUntrackedTiles(
        stream.center,
        [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
        is_available_for_far_addition
    );
    // Keep radial additions behind the forecast footprint until its deliveries are acknowledged resident.
    bool const forecast_near_has_unresident_tiles = stream.forecast_center.has_value()
        && nearWindowHasUntrackedTiles(
            *stream.forecast_center,
            [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
            [&stream](shared::HeightTileKey const key) {
                return stream.resident_keys.contains(key) && !stream.inflight_removal_keys.contains(key);
            }
        );

    std::vector<shared::HeightTileKey> ready_keys;
    ready_keys.reserve(stream.ready_tiles.size());
    for (auto const& [key, tile] : stream.ready_tiles) {
        static_cast<void>(tile);
        bool const current_near = isCurrentNearHeightTile(stream.center, key);
        bool const forecast_near = isForecastNearHeightTile(stream.forecast_center, key);
        if (stream.desires(key)
            && !stream.inflight_addition_keys.contains(key)
            && (!current_near_has_pending_additions || current_near)
            && (current_near_has_pending_additions
                || !forecast_near_has_unresident_tiles
                || current_near
                || forecast_near)) {
            ready_keys.push_back(key);
        }
    }
    auto const ready_order = [&stream](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return heightTilePriority(
            stream.center,
            stream.forecast_center,
            stream.heading_x,
            stream.heading_y,
            first
        ) < heightTilePriority(
            stream.center,
            stream.forecast_center,
            stream.heading_x,
            stream.heading_y,
            second
        );
    };
    if (ready_keys.size() > maximum_operations) {
        std::ranges::partial_sort(
            ready_keys,
            ready_keys.begin() + maximum_operations,
            ready_order
        );
        ready_keys.resize(maximum_operations);
    } else {
        std::ranges::sort(ready_keys, ready_order);
    }

    auto ready = ready_keys.begin();
    auto removal = removal_keys.begin();
    uint32_t admitted_batches = 0U;
    uint32_t remaining_ready_count = static_cast<uint32_t>(ready_keys.size());
    uint32_t remaining_removal_count = static_cast<uint32_t>(removal_keys.size());
    uint32_t const maximum_resident_tiles = shared::heightTileInterestCount(stream.render_distance);
    uint32_t const batch_capacity = shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    while (admitted_batches < available_batches
        && stream.delivery_credits > 0U
        && stream.inflight_deliveries.size() < PreviewStream::MAX_INFLIGHT_DELIVERIES
        ) {
        static constexpr uint32_t BATCH_HEADER_BYTES = 13U;
        static constexpr uint32_t REMOVAL_BYTES = 24U;
        uint32_t const byte_capacity = std::min({
            PreviewStream::MAX_DELIVERY_BYTES,
            stream_byte_limit - inflight_bytes,
            remainingTerrainDeliveryBytes(stream.client_id),
        });
        if (byte_capacity <= BATCH_HEADER_BYTES) {
            break;
        }
        uint64_t projected_resident_count = static_cast<uint64_t>(stream.resident_keys.size())
            + static_cast<uint64_t>(stream.inflight_addition_keys.size());
        uint64_t const inflight_removal_count = static_cast<uint64_t>(stream.inflight_removal_keys.size());
        projected_resident_count = projected_resident_count > inflight_removal_count
            ? projected_resident_count - inflight_removal_count
            : 0U;
        uint32_t const free_resident_capacity = projected_resident_count < maximum_resident_tiles
            ? maximum_resident_tiles - static_cast<uint32_t>(projected_resident_count)
            : 0U;
        uint32_t batch_removal_count = 0U;
        uint32_t batch_addition_count = 0U;
        if (remaining_ready_count > 0U && remaining_removal_count > 0U) {
            uint32_t const removal_progress_budget = (batch_capacity + 1U) / 2U;
            batch_removal_count = std::min(remaining_removal_count, removal_progress_budget);
            uint32_t const available_addition_slots = std::min(
                batch_capacity - batch_removal_count,
                free_resident_capacity + batch_removal_count
            );
            batch_addition_count = std::min(remaining_ready_count, available_addition_slots);
        } else if (remaining_ready_count > 0U) {
            batch_addition_count = std::min({
                remaining_ready_count,
                batch_capacity,
                free_resident_capacity,
            });
        } else {
            batch_removal_count = std::min(remaining_removal_count, batch_capacity);
        }
        batch_removal_count = std::min(
            batch_removal_count,
            (byte_capacity - BATCH_HEADER_BYTES) / REMOVAL_BYTES
        );
        if (remaining_ready_count > 0U && free_resident_capacity > 0U
            && shared::heightTileGenerationBand(stream.center, stream.heading_x, stream.heading_y, *ready)
                <= shared::HeightTileGenerationBand::Near) {
            auto const first_ready_tile = stream.ready_tiles.find(*ready);
            ASSERT(first_ready_tile != stream.ready_tiles.end(), "selected height tile is no longer ready");
            uint32_t const first_tile_bytes = shared::encodedHeightTileSize(first_ready_tile->second);
            if (first_tile_bytes <= byte_capacity - BATCH_HEADER_BYTES) {
                uint32_t const removal_capacity_with_near_tile =
                    (byte_capacity - BATCH_HEADER_BYTES - first_tile_bytes) / REMOVAL_BYTES;
                batch_removal_count = std::min(batch_removal_count, removal_capacity_with_near_tile);
            }
        }
        batch_addition_count = std::min(batch_addition_count, free_resident_capacity + batch_removal_count);
        uint32_t encoded_bytes = BATCH_HEADER_BYTES + batch_removal_count * REMOVAL_BYTES;
        uint32_t fitting_additions = 0U;
        auto addition_candidate = ready;
        while (fitting_additions < batch_addition_count) {
            auto const tile = stream.ready_tiles.find(*addition_candidate);
            ASSERT(tile != stream.ready_tiles.end(), "selected height tile is no longer ready");
            uint32_t const tile_bytes = shared::encodedHeightTileSize(tile->second);
            if (tile_bytes > byte_capacity - encoded_bytes) {
                break;
            }
            encoded_bytes += tile_bytes;
            ++fitting_additions;
            ++addition_candidate;
        }
        batch_addition_count = fitting_additions;
        if (batch_addition_count == 0U && batch_removal_count == 0U) {
            break;
        }

        shared::ServerHeightTileBatchMessage batch{
            .delivery_token = nextHeightTileToken(m_next_height_tile_token),
            .tiles = {},
            .removals = {},
        };
        PreviewStream::Delivery delivery{ .additions = {}, .removals = {}, .encoded_bytes = encoded_bytes };
        batch.tiles.reserve(batch_addition_count);
        batch.removals.reserve(batch_removal_count);
        for (uint32_t index = 0U; index < batch_removal_count; ++index) {
            shared::HeightTileKey const key = *removal;
            ++removal;
            --remaining_removal_count;
            delivery.removals.push_back(key);
            stream.inflight_removal_keys.insert(key);
            batch.removals.push_back({
                .key = key,
                .revision = PreviewStream::WORLD_REVISION,
                .token = nextHeightTileToken(m_next_height_tile_token),
            });
        }
        for (uint32_t index = 0U; index < batch_addition_count; ++index) {
            shared::HeightTileKey const key = *ready;
            ++ready;
            --remaining_ready_count;
            auto const tile = stream.ready_tiles.find(key);
            if (tile == stream.ready_tiles.end()) {
                continue;
            }
            delivery.additions.push_back(key);
            stream.inflight_addition_keys.insert(key);
            batch.tiles.push_back(tile->second);
            stream.ready_tiles.erase(tile);
        }
        if (delivery.additions.empty() && delivery.removals.empty()) {
            break;
        }
        uint64_t const delivery_token = batch.delivery_token;
        uint32_t const admitted_addition_count = static_cast<uint32_t>(delivery.additions.size());
        if (m_benchmark_metrics_enabled) {
            delivery.admitted_at = std::chrono::steady_clock::now();
        }
        stream.inflight_deliveries.emplace(delivery_token, std::move(delivery));
        inflight_bytes += encoded_bytes;
        --stream.delivery_credits;
        sendHeightTileTo(stream.client_id, std::move(batch));
        stream.admitted_additions_since_metrics += admitted_addition_count;
        ++admitted_batches;
    }
    for (; removal != removal_keys.end(); ++removal) {
        shared::HeightTileKey const key = *removal;
        stream.removal_candidates.push_back({
            .key = key,
            .priority = shared::heightTileInterestPriority(
                stream.center, stream.heading_x, stream.heading_y, key
            ),
        });
        std::ranges::push_heap(stream.removal_candidates, removal_order);
    }
    return admitted_batches;
}

void GameServer::refreshHeightTileInterest(PreviewStream& stream, shared::Player const& player)
{
    shared::HeightTileKey const next_center = heightTileKeyForPlayer(player);
    std::optional<shared::Direction> forecast_direction;
    if (PlayerReplication* const replication = playerReplication(stream.client_id);
        replication != nullptr && !replication->pending_inputs.empty()) {
        forecast_direction = replication->pending_inputs.front().direction;
    } else {
        forecast_direction = stream.last_actual_direction;
    }
    std::optional<shared::HeightTileKey> next_forecast_center;
    if (forecast_direction.has_value()) {
        std::optional<shared::PlayerPosition> const forecast_position = m_world.forecastFlightPlayerPosition(
            stream.client_id,
            *forecast_direction
        );
        if (forecast_position.has_value()) {
            next_forecast_center = heightTileKeyForCoordinates(
                forecast_position->x,
                forecast_position->y
            );
        }
    }
    shared::HeightTileHeading const heading = shared::canonicalHeightTileHeading(
        stream.heading_x, stream.heading_y
    );
    bool const forecast_changed = stream.forecast_center != next_forecast_center;
    if (stream.has_center && stream.center == next_center
        && stream.applied_heading_x == heading.x
        && stream.applied_heading_y == heading.y
        && !forecast_changed) {
        return;
    }
    auto const priority_order = std::ranges::find_if(
        m_height_tile_interest_orders->orders,
        [heading](shared::HeightTileInterest const& order) {
            return order.heading_x == heading.x && order.heading_y == heading.y;
        }
    );
    ASSERT(priority_order != m_height_tile_interest_orders->orders.end(), "invalid height-tile heading");
    bool const had_center = stream.has_center;
    bool const center_changed = !had_center || stream.center != next_center;
    bool const heading_changed = !had_center
        || stream.applied_heading_x != heading.x
        || stream.applied_heading_y != heading.y;
    shared::HeightTileInterestDelta delta;
    if (had_center && center_changed) {
        delta = shared::heightTileInterestDelta(stream.center, next_center, stream.render_distance);
    }
    stream.center = next_center;
    stream.forecast_center = next_forecast_center;
    stream.has_center = true;
    stream.applied_heading_x = heading.x;
    stream.applied_heading_y = heading.y;
    stream.priority_offsets = priority_order->keys;
    stream.priority_cursor_generation = stream.generation;
    if (center_changed || heading_changed || forecast_changed) {
        stream.priority_cursor = 0U;
        stream.removal_candidates.clear();
    }
    if (center_changed || heading_changed || forecast_changed) {
        stream.background_cursor = 0U;
    }
    if (center_changed) {
        for (shared::HeightTileKey const key : delta.additions) {
            stream.pending_removals.erase(key);
        }
        for (shared::HeightTileKey const key : delta.removals) {
            if (stream.resident_keys.contains(key) && !stream.inflight_removal_keys.contains(key)) {
                stream.pending_removals.insert(key);
            }
        }
        stream.urgent_keys = std::move(delta.additions);
        stream.urgent_cursor = 0U;
        if (stream.urgent_keys.size() > static_cast<decltype(stream.urgent_keys.size())>(
                PreviewStream::MAX_URGENT_CANDIDATES_PER_PUMP
            ) * 8U) {
            stream.urgent_keys.clear();
            stream.priority_cursor = 0U;
        }
        if (!had_center) {
            auto const now = std::chrono::steady_clock::now();
            stream.background_generation_tokens = 0.0;
            stream.background_budget_updated_at = now;
            stream.background_generation_eligible_at = now + std::chrono::milliseconds{250};
        }
    }
    if (center_changed || heading_changed || forecast_changed) {
        std::ranges::sort(stream.urgent_keys, [&stream](
            shared::HeightTileKey const first,
            shared::HeightTileKey const second
        ) {
            return heightTilePriority(
                stream.center,
                stream.forecast_center,
                stream.heading_x,
                stream.heading_y,
                first
            ) < heightTilePriority(
                stream.center,
                stream.forecast_center,
                stream.heading_x,
                stream.heading_y,
                second
            );
        });
    }

    auto const is_tracked = [&stream](shared::HeightTileKey const key) {
        return (stream.resident_keys.contains(key) && !stream.inflight_removal_keys.contains(key))
            || stream.queued_keys.contains(key)
            || stream.dispatched_keys.contains(key)
            || stream.ready_tiles.contains(key)
            || stream.inflight_addition_keys.contains(key);
    };
    bool const near_untracked = nearWindowHasUntrackedTiles(
        stream.center,
        [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
        is_tracked
    );
    bool const forecast_untracked = stream.forecast_center.has_value()
        && nearWindowHasUntrackedTiles(
            *stream.forecast_center,
            [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
            is_tracked
        );
    auto const should_cancel_key = [&stream, near_untracked, forecast_untracked](
        shared::HeightTileKey const key
    ) {
        if (!stream.desires(key)) {
            return true;
        }
        if (near_untracked) {
            return !isCurrentNearHeightTile(stream.center, key);
        }
        return forecast_untracked
            && !isCurrentNearHeightTile(stream.center, key)
            && !isForecastNearHeightTile(stream.forecast_center, key);
    };
    std::erase_if(stream.queued_keys, should_cancel_key);
    stream.scheduler.cancelQueuedIf([&should_cancel_key](shared::GenerationJob const& job) {
        return should_cancel_key({.x = job.coordinate.x, .y = job.coordinate.y});
    });
    for (shared::GenerationJob const& job : m_height_tile_workers->cancelQueuedIf(
             stream.client_id,
             [&should_cancel_key](HeightTileWorkerPool::Work const& work) {
                 return work.job.id < (uint64_t{1U} << 63U) && should_cancel_key({
                     .x = work.job.coordinate.x,
                     .y = work.job.coordinate.y,
                 });
             }
         )) {
        stream.dispatched_keys.erase({.x = job.coordinate.x, .y = job.coordinate.y});
        static_cast<void>(stream.scheduler.complete(job.id, false, true));
    }
    auto const order_jobs = [&stream](shared::GenerationJob const& first, shared::GenerationJob const& second) {
        return heightTilePriority(
            stream.center,
            stream.forecast_center,
            stream.heading_x,
            stream.heading_y,
            {.x = first.coordinate.x, .y = first.coordinate.y}
        ) < heightTilePriority(
            stream.center,
            stream.forecast_center,
            stream.heading_x,
            stream.heading_y,
            {.x = second.coordinate.x, .y = second.coordinate.y}
        );
    };
    stream.scheduler.reorderQueued(order_jobs);
    m_height_tile_workers->reorderQueued(stream.client_id, [&order_jobs](
        HeightTileWorkerPool::Work const& first,
        HeightTileWorkerPool::Work const& second
    ) {
        return order_jobs(first.job, second.job);
    });

    std::erase_if(stream.ready_tiles, [&stream, near_untracked, forecast_untracked](auto const& entry) {
        if (!stream.desires(entry.first)) {
            return true;
        }
        if (near_untracked) {
            return !isCurrentNearHeightTile(stream.center, entry.first);
        }
        return forecast_untracked
            && !isCurrentNearHeightTile(stream.center, entry.first)
            && !isForecastNearHeightTile(stream.forecast_center, entry.first);
    });
    fillHeightTileQueue(stream);
}

void GameServer::fillHeightTileQueue(PreviewStream& stream)
{
    if (stream.priority_cursor_generation != stream.generation) {
        stream.priority_cursor = 0U;
        stream.priority_cursor_generation = stream.generation;
    }
    if (stream.background_cursor_generation != stream.generation) {
        stream.background_cursor = 0U;
        stream.background_cursor_generation = stream.generation;
    }
    uint32_t queued_submissions = 0U;
    auto const reorder_queued = [&stream, this] {
        auto const order_jobs = [&stream](shared::GenerationJob const& first, shared::GenerationJob const& second) {
            return heightTilePriority(
                stream.center,
                stream.forecast_center,
                stream.heading_x,
                stream.heading_y,
                {.x = first.coordinate.x, .y = first.coordinate.y}
            ) < heightTilePriority(
                stream.center,
                stream.forecast_center,
                stream.heading_x,
                stream.heading_y,
                {.x = second.coordinate.x, .y = second.coordinate.y}
            );
        };
        stream.scheduler.reorderQueued(order_jobs);
        m_height_tile_workers->reorderQueued(stream.client_id, [&order_jobs](
            HeightTileWorkerPool::Work const& first,
            HeightTileWorkerPool::Work const& second
        ) {
            return order_jobs(first.job, second.job);
        });
    };
    auto const reorder_if_submitted = [&] {
        if (queued_submissions > 0U) {
            reorder_queued();
        }
    };
    auto const enqueue = [&stream, &queued_submissions](
        shared::HeightTileKey const key,
        uint32_t& submissions
    ) {
        if (stream.resident_keys.contains(key)
            || stream.queued_keys.contains(key)
            || stream.dispatched_keys.contains(key)
            || stream.ready_tiles.contains(key)
            || stream.inflight_addition_keys.contains(key)) {
            return true;
        }
        shared::GenerationAdmission const admission = stream.scheduler.submit(
            {.x = key.x, .y = key.y, .z = 0},
            stream.generation,
            shared::GenerationStage::HeightTile
        );
        if (admission == shared::GenerationAdmission::QueueFull) {
            return false;
        }
        if (admission == shared::GenerationAdmission::Accepted) {
            stream.queued_keys.insert(key);
            ++submissions;
            ++queued_submissions;
        }
        return true;
    };
    uint32_t const initial_slots = PreviewStream::MAX_QUEUED_TILES
        - static_cast<uint32_t>(stream.scheduler.pendingCount());
    auto const is_tracked = [&stream](shared::HeightTileKey const key) {
        return (stream.resident_keys.contains(key) && !stream.inflight_removal_keys.contains(key))
            || stream.queued_keys.contains(key)
            || stream.dispatched_keys.contains(key)
            || stream.ready_tiles.contains(key)
            || stream.inflight_addition_keys.contains(key);
    };
    bool const near_untracked = nearWindowHasUntrackedTiles(
        stream.center,
        [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
        is_tracked
    );
    bool const forecast_untracked = stream.forecast_center.has_value()
        && nearWindowHasUntrackedTiles(
            *stream.forecast_center,
            [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
            is_tracked
        );
    if (near_untracked) {
        std::erase_if(stream.ready_tiles, [&stream](auto const& entry) {
            return !stream.desires(entry.first)
                || !isCurrentNearHeightTile(stream.center, entry.first);
        });
    } else if (forecast_untracked) {
        std::erase_if(stream.ready_tiles, [&stream](auto const& entry) {
            return !stream.desires(entry.first)
                || (!isCurrentNearHeightTile(stream.center, entry.first)
                    && !isForecastNearHeightTile(stream.forecast_center, entry.first));
        });
    }
    if (stream.scheduler.pendingCount() >= PreviewStream::MAX_QUEUED_TILES
        || stream.priority_offsets.empty()) {
        return;
    }
    if (near_untracked) {
        uint32_t near_submissions = 0U;
        for (int32_t y = -CURRENT_NEAR_WINDOW_RADIUS; y <= CURRENT_NEAR_WINDOW_RADIUS; ++y) {
            for (int32_t x = -CURRENT_NEAR_WINDOW_RADIUS; x <= CURRENT_NEAR_WINDOW_RADIUS; ++x) {
                shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                    .x = stream.center.x + x,
                    .y = stream.center.y + y,
                });
                if (stream.desires(key) && !enqueue(key, near_submissions)) {
                    reorder_if_submitted();
                    return;
                }
                if (near_submissions >= initial_slots) {
                    reorder_if_submitted();
                    return;
                }
            }
        }
        reorder_if_submitted();
        return;
    }
    if (stream.forecast_center.has_value() && forecast_untracked) {
        uint32_t forecast_submissions = 0U;
        for (int32_t y = -CURRENT_NEAR_WINDOW_RADIUS; y <= CURRENT_NEAR_WINDOW_RADIUS; ++y) {
            for (int32_t x = -CURRENT_NEAR_WINDOW_RADIUS; x <= CURRENT_NEAR_WINDOW_RADIUS; ++x) {
                shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                    .x = stream.forecast_center->x + x,
                    .y = stream.forecast_center->y + y,
                });
                if (stream.desires(key) && !enqueue(key, forecast_submissions)) {
                    reorder_if_submitted();
                    return;
                }
            }
        }
        bool const forecast_still_untracked = nearWindowHasUntrackedTiles(
            *stream.forecast_center,
            [&stream](shared::HeightTileKey const key) { return stream.desires(key); },
            is_tracked
        );
        if (forecast_still_untracked) {
            reorder_if_submitted();
            return;
        }
    }
    uint32_t const urgent_submission_limit = stream.urgent_cursor < stream.urgent_keys.size()
        ? std::min<uint32_t>(64U, std::max<uint32_t>(1U, initial_slots / 2U))
        : 0U;
    uint32_t urgent_submissions = 0U;
    uint32_t urgent_inspections = 0U;
    while (stream.urgent_cursor < stream.urgent_keys.size()
        && urgent_inspections < PreviewStream::MAX_URGENT_CANDIDATES_PER_PUMP
        && urgent_submissions < urgent_submission_limit
        && stream.scheduler.pendingCount() < PreviewStream::MAX_QUEUED_TILES) {
        shared::HeightTileKey const key = stream.urgent_keys[stream.urgent_cursor];
        ++urgent_inspections;
        if (stream.desires(key) && !enqueue(key, urgent_submissions)) {
            reorder_if_submitted();
            return;
        }
        ++stream.urgent_cursor;
    }
    if (stream.urgent_cursor >= stream.urgent_keys.size()) {
        stream.urgent_keys.clear();
        stream.urgent_cursor = 0U;
    }
    uint32_t const offset_count = static_cast<uint32_t>(stream.priority_offsets.size());
    uint32_t const slots_after_urgent = PreviewStream::MAX_QUEUED_TILES
        - static_cast<uint32_t>(stream.scheduler.pendingCount());
    uint32_t const priority_submission_limit = slots_after_urgent > 0U
        ? std::min<uint32_t>(32U, std::max<uint32_t>(1U, slots_after_urgent / 2U))
        : 0U;
    uint32_t priority_submissions = 0U;
    for (uint32_t inspected = 0U;
         inspected < PreviewStream::MAX_PRIORITY_CANDIDATES_PER_PUMP
             && priority_submissions < priority_submission_limit
             && stream.scheduler.pendingCount() < PreviewStream::MAX_QUEUED_TILES;
         ++inspected) {
        if (stream.priority_cursor >= offset_count) {
            stream.priority_cursor = 0U;
        }
        shared::HeightTileKey const offset = stream.priority_offsets[stream.priority_cursor];
        shared::HeightTileKey const key = shared::normalizeHeightTileKey({
            .x = stream.center.x + offset.x,
            .y = stream.center.y + offset.y,
        });
        if (!enqueue(key, priority_submissions)) {
            reorder_if_submitted();
            return;
        }
        ++stream.priority_cursor;
    }
    uint32_t background_submissions = 0U;
    for (uint32_t inspected = 0U;
         inspected < PreviewStream::MAX_BACKGROUND_CANDIDATES_PER_PUMP
             && stream.scheduler.pendingCount() < PreviewStream::MAX_QUEUED_TILES;
         ++inspected) {
        if (stream.background_cursor >= offset_count) {
            stream.background_cursor = 0U;
        }
        shared::HeightTileKey const offset = stream.priority_offsets[stream.background_cursor];
        shared::HeightTileKey const key = shared::normalizeHeightTileKey({
            .x = stream.center.x + offset.x,
            .y = stream.center.y + offset.y,
        });
        if (!enqueue(key, background_submissions)) {
            reorder_if_submitted();
            return;
        }
        ++stream.background_cursor;
    }
    reorder_if_submitted();
}

bool GameServer::publishAuthoritativeTerrainResult(HeightTileWorkerPool::Result const& result)
{
    if (result.world_generation || result.job.id < (uint64_t{1U} << 63U)) {
        return false;
    }
    PlayerReplication* const replication = playerReplication(result.client_id);
    if (replication == nullptr) {
        return true;
    }
    auto const pending = replication->authoritative_jobs.find(result.job.id);
    if (pending == replication->authoritative_jobs.end()) {
        return true;
    }
    shared::ChunkCoordinate const coordinate = pending->second;
    replication->scheduled_authoritative_columns.erase(coordinate);
    replication->authoritative_jobs.erase(pending);
    if (m_terrain_world == nullptr || m_physics_world == nullptr
        || !replication->authoritative_interest_columns.contains(coordinate)) {
        return true;
    }
    bool const canonical = result.succeeded && !result.cancelled
        && isCanonicalHeightTile(result.tile, m_terrain_world->identity(), coordinate);
    bool const published = canonical && m_physics_world->publishAuthoritativeSurface(result.tile.surface);
    if (!published) {
        auto const key = std::ranges::find(replication->ordered_authoritative_columns, coordinate);
        uint32_t const retry_index = static_cast<uint32_t>(key - replication->ordered_authoritative_columns.begin());
        replication->next_authoritative_generation_index = std::min(
            replication->next_authoritative_generation_index, retry_index
        );
        ++replication->authoritative_publication_failures;
        if (replication->authoritative_publication_failures == 1U) {
            CORE_ERROR("Authoritative profile rejected x={} y={} worker_succeeded={} cancelled={} canonical={} "
                "resident_profiles={} identity={}", coordinate.x, coordinate.y, result.succeeded,
                result.cancelled, canonical, m_physics_world->availableAuthoritativeColumns().size(),
                result.tile.surface.identity == m_terrain_world->identity());
        }
    }
    return true;
}

void GameServer::acknowledgeAuthoritativeTerrain(
    core::ClientId const client_id,
    shared::ClientAuthoritativeTerrainAckMessage const& acknowledgement
)
{
    PlayerReplication* const replication = playerReplication(client_id);
    if (replication == nullptr || replication->terrain_session != acknowledgement.session) {
        return;
    }
    auto const delivery = replication->authoritative_deliveries.find(acknowledgement.delivery_token);
    if (delivery == replication->authoritative_deliveries.end()
        || delivery->second.coverage_epoch != acknowledgement.coverage_epoch) {
        return;
    }
    if (acknowledgement.coverage_epoch != replication->coverage_epoch) {
        replication->authoritative_deliveries.erase(delivery);
        return;
    }
    if (std::ranges::any_of(acknowledgement.not_resident_keys, [&](auto const key) {
            return std::ranges::find(delivery->second.coordinates, key) == delivery->second.coordinates.end();
        })) {
        return;
    }
    for (shared::ChunkCoordinate const key : delivery->second.coordinates) {
        replication->inflight_authoritative_columns.erase(key);
        if (replication->authoritative_interest_columns.contains(key)
            && std::ranges::find(acknowledgement.not_resident_keys, key)
                == acknowledgement.not_resident_keys.end()) {
            replication->acknowledged_authoritative_columns.insert(key);
        }
    }
    replication->authoritative_deliveries.erase(delivery);
    for (shared::ChunkCoordinate const key : acknowledgement.not_resident_keys) {
        auto const found = std::ranges::find(replication->ordered_authoritative_columns, key);
        if (found == replication->ordered_authoritative_columns.end()) {
            continue;
        }
        uint32_t const index = static_cast<uint32_t>(found - replication->ordered_authoritative_columns.begin());
        if (std::ranges::find(replication->authoritative_delivery_frontier, index)
            == replication->authoritative_delivery_frontier.end()) {
            replication->authoritative_delivery_frontier.push_front(index);
        }
    }
}

void GameServer::processAuthoritativeTerrain()
{
    if (m_height_tile_workers == nullptr || m_terrain_world == nullptr || m_physics_world == nullptr) {
        return;
    }
    static constexpr uint32_t MAX_JOBS_PER_PLAYER = 32U;
    static constexpr uint32_t MAX_BATCHES_PER_PUMP = 4U;
    for (PlayerReplication& replication : m_player_replications) {
        if (!replication.terrain_session || !replication.has_chunk_coverage) {
            continue;
        }
        uint32_t admitted = 0U;
        shared::ChunkCoordinate root = shared::WorldBounds::chunkCoordinate({
            replication.coverage_center.x, replication.coverage_center.y, 0,
        });
        PlayerReplication::ChunkKeySet connected = replication.acknowledged_authoritative_columns;
        connected.insert(replication.inflight_authoritative_columns.begin(),
            replication.inflight_authoritative_columns.end());
        shared::ServerAuthoritativeTerrainBatchMessage batch{
            .session = *replication.terrain_session, .coverage_epoch = replication.coverage_epoch,
            .delivery_token = replication.next_authoritative_delivery_token,
            .tiles = {},
        };
        uint32_t inflight_bytes = 0U;
        for (auto const& [token, delivery] : replication.authoritative_deliveries) {
            static_cast<void>(token);
            inflight_bytes += delivery.encoded_bytes;
        }
        auto const send_batch = [&] {
            if (batch.tiles.empty()) {
                return;
            }
            std::vector<shared::ChunkCoordinate> keys;
            keys.reserve(batch.tiles.size());
            for (auto const& tile : batch.tiles) {
                keys.push_back(tile.coordinate);
                replication.inflight_authoritative_columns.insert(tile.coordinate);
            }
            uint32_t const encoded_bytes = static_cast<uint32_t>(shared::encodedAuthoritativeTerrainBatchSize(batch));
            replication.authoritative_deliveries.emplace(batch.delivery_token, PlayerReplication::TerrainDelivery{
                .coordinates = std::move(keys), .encoded_bytes = encoded_bytes,
                .coverage_epoch = replication.coverage_epoch,
            });
            inflight_bytes += encoded_bytes;
            sendTo(replication.id, std::move(batch));
            ++replication.next_authoritative_delivery_token;
            ++admitted;
            batch = { .session = *replication.terrain_session, .coverage_epoch = replication.coverage_epoch,
                .delivery_token = replication.next_authoritative_delivery_token, .tiles = {} };
        };
        static constexpr uint32_t MAX_GENERATION_CANDIDATES_PER_PUMP = 256U;
        uint32_t inspected = 0U;
        while (replication.next_authoritative_generation_index < replication.ordered_authoritative_columns.size()
            && inspected++ < MAX_GENERATION_CANDIDATES_PER_PUMP) {
            shared::ChunkCoordinate const coordinate = replication.ordered_authoritative_columns[
                replication.next_authoritative_generation_index
            ];
            if (m_physics_world->hasAuthoritativeSurface(coordinate)
                || replication.scheduled_authoritative_columns.contains(coordinate)) {
                ++replication.next_authoritative_generation_index;
                continue;
            }
            if (replication.authoritative_jobs.size() >= MAX_JOBS_PER_PLAYER
                || !m_height_tile_workers->canAccept()) {
                break;
            }
            shared::GenerationJob job{
                .id = m_next_authoritative_job_id++, .coordinate = coordinate,
                .revision = m_terrain_world->identity().revision, .seed = m_terrain_world->identity().seed,
                .region = std::nullopt,
                .inherited_ancestor_data = nullptr,
                .terrain_identity = m_terrain_world->identity(), .world_epoch = SERVER_TERRAIN_WORLD_EPOCH,
                .attempt = 1U,
                .cancellation_source = std::make_shared<std::stop_source>(),
            };
            auto const bound = m_world_generation.bindTerrainLeaf(job);
            if (!bound) {
                auto plan = m_generation_plan;
                plan.chunk_stages = { shared::GenerationStage::HeightTile };
                static_cast<void>(m_world_generation.requestPlan(coordinate, std::move(plan)));
                ++replication.next_authoritative_generation_index;
                continue;
            }
            if (m_height_tile_workers->enqueue({ .client_id = replication.id,
                    .generation = job.revision, .job = *bound })) {
                replication.authoritative_jobs.emplace(job.id, coordinate);
                replication.scheduled_authoritative_columns.insert(coordinate);
            }
            if (replication.scheduled_authoritative_columns.contains(coordinate)) {
                ++replication.next_authoritative_generation_index;
            } else {
                break;
            }
        }
        static constexpr uint32_t MAX_DELIVERY_CANDIDATES_PER_PUMP = 256U;
        if (!replication.chunk_credit_grant_received) {
            continue;
        }
        uint32_t const delivery_limit = std::min(MAX_DELIVERY_CANDIDATES_PER_PUMP,
            static_cast<uint32_t>(replication.authoritative_delivery_frontier.size()));
        uint32_t delivery_inspected = 0U;
        while (!replication.authoritative_delivery_frontier.empty() && delivery_inspected < delivery_limit) {
            if (replication.authoritative_deliveries.size() >= shared::AUTHORITATIVE_TERRAIN_DELIVERY_WINDOW) {
                break;
            }
            uint32_t const delivery_index = replication.authoritative_delivery_frontier.front();
            replication.authoritative_delivery_frontier.pop_front();
            replication.next_authoritative_delivery_index = delivery_index;
            ++delivery_inspected;
            shared::ChunkCoordinate const coordinate = replication.ordered_authoritative_columns[delivery_index];
            if (replication.acknowledged_authoritative_columns.contains(coordinate)
                || replication.inflight_authoritative_columns.contains(coordinate)) {
                continue;
            }
            if (!faceConnectedTo(coordinate, connected, root)) {
                replication.authoritative_delivery_frontier.push_back(delivery_index);
                continue;
            }
            auto tile = m_physics_world->authoritativeTile(coordinate);
            if (!tile) {
                replication.authoritative_delivery_frontier.push_back(delivery_index);
                replication.next_authoritative_generation_index = std::min(
                    replication.next_authoritative_generation_index, delivery_index
                );
                continue;
            }
            batch.tiles.push_back(std::move(*tile));
            uint32_t const byte_capacity = std::min(
                MAX_PREPARED_STREAM_INFLIGHT_BYTES - inflight_bytes,
                remainingTerrainDeliveryBytes(replication.id)
            );
            if (shared::encodedAuthoritativeTerrainBatchSize(batch) > byte_capacity) {
                batch.tiles.pop_back();
                replication.authoritative_delivery_frontier.push_front(delivery_index);
                break;
            }
            connected.insert(coordinate);
            if (batch.tiles.size() == shared::AUTHORITATIVE_TERRAIN_BATCH_CAPACITY) {
                send_batch();
                if (admitted >= MAX_BATCHES_PER_PUMP) {
                    break;
                }
            }
        }
        if (replication.authoritative_deliveries.size() < shared::AUTHORITATIVE_TERRAIN_DELIVERY_WINDOW) {
            send_batch();
        }
    }
}

void GameServer::processHeightTileStreams()
{
    processWorldPreparation();
    if (m_height_tile_workers == nullptr) {
        return;
    }
    auto const started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    refreshMaterializedCoverage();
    for (PreviewStream& stream : m_preview_streams) {
        auto const player = m_world.player(stream.client_id);
        if (player) {
            refreshHeightTileInterest(stream, *player);
        }
    }
    if (m_benchmark_metrics_enabled) {
        ++m_streaming_metrics.coverage_refresh.calls;
        m_streaming_metrics.coverage_refresh.duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - phase_started_at
        );
    }

    phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    publishHeightTileResults();
    if (m_benchmark_metrics_enabled) {
        ++m_streaming_metrics.worker_result_publication.calls;
        m_streaming_metrics.worker_result_publication.duration +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - phase_started_at
            );
    }

    phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    fillMaterializedChunkQueue();
    for (PreviewStream& stream : m_preview_streams) {
        fillHeightTileQueue(stream);
    }
    if (m_benchmark_metrics_enabled) {
        ++m_streaming_metrics.queue_refill.calls;
        m_streaming_metrics.queue_refill.duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - phase_started_at
        );
    }

    phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!m_preview_streams.empty()) {
        static constexpr uint32_t MAX_ADMITTED_BATCHES_PER_PUMP = 4U;
        uint32_t remaining_batches = MAX_ADMITTED_BATCHES_PER_PUMP;
        size_t const stream_count = m_preview_streams.size();
        size_t round_start = m_next_preview_admission % stream_count;
        while (remaining_batches > 0U) {
            bool admitted = false;
            for (size_t offset = 0U; offset < stream_count && remaining_batches > 0U; ++offset) {
                size_t const index = (round_start + offset) % stream_count;
                uint32_t const admitted_batches = admitHeightTileDeliveries(
                    m_preview_streams[index], 1U
                );
                remaining_batches -= admitted_batches;
                admitted = admitted || admitted_batches != 0U;
            }
            if (!admitted) {
                break;
            }
            round_start = (round_start + 1U) % stream_count;
        }
        m_next_preview_admission = (m_next_preview_admission + 1U) % stream_count;
    }
    if (m_benchmark_metrics_enabled) {
        m_streaming_metrics.delivery.duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - phase_started_at
        );
    }

    phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    dispatchWorldMaterialization();
    if (m_benchmark_metrics_enabled) {
        m_streaming_metrics.worker_dispatch.duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - phase_started_at
        );
    }

    phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    pumpMaterializedChunkDeliveries();
    if (m_benchmark_metrics_enabled) {
        ++m_streaming_metrics.delivery.calls;
        m_streaming_metrics.delivery.duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - phase_started_at
        );
    }

    phase_started_at = m_benchmark_metrics_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    processAuthoritativeTerrain();
    dispatchHeightTileWork();
    if (m_benchmark_metrics_enabled) {
        ++m_streaming_metrics.worker_dispatch.calls;
        m_streaming_metrics.worker_dispatch.duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - phase_started_at
        );
    }
    if (m_benchmark_metrics_enabled) {
        m_last_stream_pump_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at
        );
    }
}

void GameServer::dispatchWorldMaterialization()
{
    if (m_height_tile_workers == nullptr || m_physics_world == nullptr) {
        return;
    }
    static constexpr uint32_t MAX_DISPATCHED_PER_PUMP = 48U;
    uint32_t dispatched = 0U;
    while (dispatched < MAX_DISPATCHED_PER_PUMP && m_height_tile_workers->canAccept()) {
        auto const job = m_world_generation.takeNext();
        if (!job.has_value()) {
            break;
        }
        if (!m_height_tile_workers->enqueue({
                .client_id = 0U,
                .generation = job->revision,
                .job = *job,
                .world_generation = true,
            })) {
            if (m_terrain_world != nullptr) {
                static_cast<void>(m_world_generation.cancel(
                    job->coordinate,
                    job->revision,
                    job->seed
                ));
            } else {
                static_cast<void>(m_world_generation.complete(job->id, false));
                static_cast<void>(m_world_generation.takeResult());
            }
            break;
        }
        ++dispatched;
        if (m_benchmark_metrics_enabled) {
            ++m_streaming_metrics.world_generation_jobs_dispatched;
        }
    }
}

void GameServer::dispatchHeightTileWork()
{
    if (m_height_tile_workers == nullptr) {
        return;
    }
    static constexpr uint32_t MAX_DISPATCHED_TILES_PER_PUMP = PreviewStream::MAX_QUEUED_TILES;
    static constexpr double BACKGROUND_TILES_PER_SECOND = 32'768.0;
    static constexpr double MAXIMUM_BACKGROUND_BURST = 128.0;
    auto const now = std::chrono::steady_clock::now();
    for (PreviewStream& stream : m_preview_streams) {
        double const elapsed_seconds = now >= stream.background_generation_eligible_at
            ? std::chrono::duration<double>(now - std::max(
                stream.background_budget_updated_at,
                stream.background_generation_eligible_at
            )).count()
            : 0.0;
        stream.background_budget_updated_at = now;
        stream.background_generation_tokens = std::min(
            MAXIMUM_BACKGROUND_BURST,
            stream.background_generation_tokens + elapsed_seconds * BACKGROUND_TILES_PER_SECOND
        );
    }
    if (m_preview_streams.empty()) {
        return;
    }

    uint32_t dispatched = 0U;
    size_t const stream_count = m_preview_streams.size();
    size_t round_start = m_next_preview_dispatch % stream_count;
    while (dispatched < MAX_DISPATCHED_TILES_PER_PUMP && m_height_tile_workers->canAccept()) {
        bool dispatched_round = false;
        for (size_t offset = 0U; offset < stream_count
            && dispatched < MAX_DISPATCHED_TILES_PER_PUMP
            && m_height_tile_workers->canAccept(); ++offset) {
            PreviewStream& stream = m_preview_streams[(round_start + offset) % stream_count];
            if (stream.ready_tiles.size() + stream.dispatched_keys.size() >= PreviewStream::MAX_BUFFERED_TILES) {
                continue;
            }
            auto const next = stream.scheduler.peekNext();
            if (!next.has_value()) {
                continue;
            }
            bool const background = shared::heightTileGenerationBand(
                stream.center,
                stream.heading_x,
                stream.heading_y,
                {.x = next->coordinate.x, .y = next->coordinate.y}
            ) == shared::HeightTileGenerationBand::Background
                && !isForecastNearHeightTile(
                    stream.forecast_center,
                    {.x = next->coordinate.x, .y = next->coordinate.y}
                );
            if (background && stream.background_generation_tokens < 1.0) {
                continue;
            }
            std::optional<shared::GenerationJob> job;
            if (m_terrain_world != nullptr) {
                std::optional<shared::GenerationJob> bound = m_world_generation.bindTerrainLeaf(*next);
                if (!bound.has_value()) {
                    shared::WorldGenerationPlan surface_plan = m_generation_plan;
                    surface_plan.chunk_stages = {shared::GenerationStage::HeightTile};
                    static_cast<void>(m_world_generation.requestPlan(
                        {.x = next->coordinate.x, .y = next->coordinate.y, .z = 0},
                        std::move(surface_plan)
                    ));
                    continue;
                }
                job = stream.scheduler.takeNext();
                if (!job.has_value() || job->id != bound->id) {
                    continue;
                }
                job = std::move(bound);
            } else {
                job = stream.scheduler.takeNext();
            }
            if (!job.has_value()) {
                continue;
            }
            shared::HeightTileKey const key{.x = job->coordinate.x, .y = job->coordinate.y};
            stream.queued_keys.erase(key);
            if (!m_height_tile_workers->enqueue({
                .client_id = stream.client_id,
                .generation = stream.generation,
                .job = *job,
            })) {
                static_cast<void>(stream.scheduler.complete(*job, false, true));
                stream.priority_cursor = 0U;
                break;
            }
            stream.dispatched_keys.insert(key);
            if (background) {
                stream.background_generation_tokens -= 1.0;
            }
            ++dispatched;
            if (m_benchmark_metrics_enabled) {
                ++m_streaming_metrics.height_tile_jobs_dispatched;
            }
            dispatched_round = true;
        }
        if (!dispatched_round) {
            break;
        }
        round_start = (round_start + 1U) % stream_count;
    }
    m_next_preview_dispatch = (m_next_preview_dispatch + 1U) % stream_count;
}

void GameServer::publishHeightTileResults()
{
    if (m_height_tile_workers == nullptr || m_physics_world == nullptr) {
        return;
    }
    static constexpr uint32_t MAX_PUBLISHED_RESULTS_PER_PUMP = 4U * shared::HEIGHT_TILE_BATCH_CAPACITY;
    std::vector<HeightTileWorkerPool::Result> results = m_height_tile_workers->takeResults(
        MAX_PUBLISHED_RESULTS_PER_PUMP
    );
    std::ranges::stable_sort(results, [this](
        HeightTileWorkerPool::Result const& first,
        HeightTileWorkerPool::Result const& second
    ) {
        if (first.world_generation != second.world_generation) {
            return first.world_generation;
        }
        if (first.world_generation) {
            return std::tie(first.job.revision, first.job.coordinate.z, first.job.coordinate.y,
                first.job.coordinate.x, first.job.id)
                < std::tie(second.job.revision, second.job.coordinate.z, second.job.coordinate.y,
                    second.job.coordinate.x, second.job.id);
        }
        if (first.client_id != second.client_id) {
            return first.client_id < second.client_id;
        }
        if (first.generation != second.generation) {
            return first.generation < second.generation;
        }
        auto const stream = std::ranges::find(m_preview_streams, first.client_id, &PreviewStream::client_id);
        if (stream == m_preview_streams.end()) {
            return false;
        }
        auto const priority = [&stream](shared::GenerationJob const& job) {
            return heightTilePriority(
                stream->center,
                stream->forecast_center,
                stream->heading_x,
                stream->heading_y,
                {.x = job.coordinate.x, .y = job.coordinate.y}
            );
        };
        return priority(first.job) < priority(second.job);
    });
    for (HeightTileWorkerPool::Result& result : results) {
        if (publishAuthoritativeTerrainResult(result)) {
            continue;
        }
        if (result.world_generation) {
            for (PlayerReplication& replication : m_player_replications) {
                replication.scheduled_chunks.erase(result.job.coordinate);
            }
            bool const typed_context = m_terrain_world != nullptr;
            bool const current_identity = result.job.revision == m_world.configuration().algorithm_version
                && result.job.seed == m_world.configuration().seed
                && (!typed_context
                    || (result.job.terrain_identity == m_terrain_world->identity()
                        && result.job.world_epoch == SERVER_TERRAIN_WORLD_EPOCH));
            bool const regional = result.job.stage == shared::GenerationStage::Pregen
                || result.job.stage == shared::GenerationStage::Refinement;
            bool const cancellation = !current_identity || result.cancelled;
            if (typed_context && !cancellation
                && result.succeeded
                && (result.job.stage == shared::GenerationStage::HeightTile
                    || result.job.stage == shared::GenerationStage::Materialize)
                && !isCanonicalHeightTile(result.tile, m_terrain_world->identity(), result.job.coordinate)) {
                static_cast<void>(m_world_generation.complete(result.job, false, false));
                auto const rejected = m_world_generation.takeResult();
                if (rejected.has_value() && !rejected->cancelled) {
                    static_cast<void>(m_world_generation.retry(result.job.id));
                }
                continue;
            }
            if (typed_context && current_identity && result.succeeded && !result.cancelled
                && (result.job.stage == shared::GenerationStage::HeightTile
                    || result.job.stage == shared::GenerationStage::Materialize)) {
                if (!m_physics_world->publishAuthoritativeSurface(result.tile.surface)) {
                    for (PlayerReplication& replication : m_player_replications) {
                        if (!replication.authoritative_interest_columns.contains(result.tile.surface.coordinate)) {
                            continue;
                        }
                        replication.next_authoritative_generation_index = std::min(
                            replication.next_authoritative_generation_index,
                            replication.next_authoritative_delivery_index
                        );
                    }
                }
            }
            bool completed = false;
            if (typed_context && regional) {
                bool const valid_artifact = result.succeeded
                    && result.regional_artifact != nullptr
                    && result.regional_artifact->identity() == m_terrain_world->identity();
                completed = m_world_generation.completeRegional(
                    result.job,
                    valid_artifact && !cancellation,
                    cancellation,
                    valid_artifact && !cancellation ? result.regional_artifact : nullptr
                );
            } else if (typed_context) {
                completed = m_world_generation.complete(
                    result.job,
                    result.succeeded && !cancellation,
                    cancellation
                );
            } else {
                std::vector<uint8_t> output = regional
                    ? std::move(result.generation_output)
                    : std::vector<uint8_t>{};
                completed = m_world_generation.complete(
                    result.job.id,
                    result.succeeded && !cancellation,
                    cancellation,
                    std::move(output)
                );
            }
            if (!completed) {
                continue;
            }
            auto const generated = m_world_generation.takeResult();
            if (!generated.has_value() || generated->job.id != result.job.id) {
                continue;
            }
            if (!generated->succeeded) {
                if (typed_context && !generated->cancelled) {
                    static_cast<void>(m_world_generation.retry(result.job.id));
                }
                continue;
            }
            if (result.job.stage == shared::GenerationStage::Materialize) {
                if (m_physics_world->publishMaterializedChunk(
                        result.chunk, result.job.revision, result.job.seed
                    )) {
                    for (PlayerReplication& replication : m_player_replications) {
                        if (replication.waiting_delivery_frontier.erase(result.job.coordinate) > 0U) {
                            enqueueMaterializedDeliveryCandidate(replication, result.job.coordinate);
                        }
                    }
                    static_cast<void>(m_world_generation.acceptResult(result.job.id));
                } else {
                    static_cast<void>(m_world_generation.cancel(
                        result.job.coordinate, result.job.revision, result.job.seed
                    ));
                }
                continue;
            }
            if (result.job.stage == shared::GenerationStage::HeightTile) {
                if (typed_context) {
                    shared::HeightTileKey const key{
                        .x = result.job.coordinate.x,
                        .y = result.job.coordinate.y,
                    };
                    for (PendingTerrainJoin& join : m_pending_terrain_joins) {
                        if (join.requested_surface_key == key) {
                            join.surface_key = key;
                            join.surface_tile = result.tile.surface;
                        }
                    }
                }
                if (!m_physics_world->publishPreview(
                        result.job.coordinate,
                        result.tile,
                        result.job.revision,
                        result.job.seed
                    )) {
                    static_cast<void>(m_world_generation.cancel(
                        result.job.coordinate, result.job.revision, result.job.seed
                    ));
                    continue;
                }
            }
            static_cast<void>(m_world_generation.acceptResult(result.job.id));
            continue;
        }
        auto const stream = std::ranges::find(m_preview_streams, result.client_id, &PreviewStream::client_id);
        if (stream == m_preview_streams.end()) {
            continue;
        }
        bool const typed_result_mismatch = m_terrain_world != nullptr
            && (result.job.terrain_identity != m_terrain_world->identity()
                || result.job.world_epoch != SERVER_TERRAIN_WORLD_EPOCH
                || (result.succeeded
                    && !isCanonicalHeightTile(
                        result.tile,
                        m_terrain_world->identity(),
                        result.job.coordinate
                    )));
        if (stream->generation != result.generation || result.job.revision != result.generation
            || typed_result_mismatch) {
            stream->dispatched_keys.erase({.x = result.job.coordinate.x, .y = result.job.coordinate.y});
            static_cast<void>(stream->scheduler.complete(result.job, false, true));
            continue;
        }
        shared::HeightTileKey const key{.x = result.job.coordinate.x, .y = result.job.coordinate.y};
        stream->dispatched_keys.erase(key);
        if (!stream->scheduler.complete(result.job, result.succeeded, result.cancelled)) {
            continue;
        }
        auto const scheduler_result = stream->scheduler.takeResult();
        if (!scheduler_result || scheduler_result->job.id != result.job.id) {
            continue;
        }
        if (!scheduler_result->succeeded) {
            if (stream->scheduler.retry(result.job.id)) {
                stream->queued_keys.insert(key);
            } else {
                stream->priority_cursor = 0U;
            }
            continue;
        }
        if (!stream->desires(key) || stream->resident_keys.contains(key)
            || stream->inflight_addition_keys.contains(key)) {
            continue;
        }
        shared::ServerHeightTileMessage tile{
            .key = key,
            .revision = PreviewStream::WORLD_REVISION,
            .token = nextHeightTileToken(m_next_height_tile_token),
            .heights = result.tile.heights,
            .surface = result.tile.surface,
        };
        stream->ready_tiles.insert_or_assign(key, tile);
    }
}

} // namespace server
