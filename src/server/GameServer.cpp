#include <server/GameServer.hpp>

#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/IO/Log.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace {


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
shared::HeightTileKey heightTileKeyForPlayer(shared::Player const& player) noexcept
{
    return shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(player.x),
        .y = floorDivideByHeightTileSide(player.y),
    });
}

[[nodiscard]]
std::tuple<double, int32_t, int32_t> heightTilePriority(
    shared::HeightTileKey const center,
    int32_t const movement_x,
    int32_t const movement_y,
    shared::HeightTileKey const key
) noexcept
{
    return {
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

struct GameServer::HeightTileWorkerPool final {
    static constexpr uint32_t MAX_OUTSTANDING_WORK = 128U;

    struct Work final {
        core::ClientId client_id;
        uint64_t generation;
        shared::GenerationJob job;
    };

    struct Result final {
        core::ClientId client_id;
        uint64_t generation;
        shared::GenerationJob job;
        bool succeeded = false;
        shared::HeightTile tile{};
    };

    HeightTileWorkerPool()
    {
        uint32_t const worker_count = GameServer::terrainWorkerCount(std::thread::hardware_concurrency());
        m_workers.reserve(worker_count);
        for (uint32_t worker{ 0U }; worker < worker_count; ++worker) {
            m_workers.emplace_back([this] {
                workerLoop();
            });
        }
    }

    ~HeightTileWorkerPool()
    {
        {
            std::lock_guard lock{m_mutex};
            m_stopping = true;
            m_work.clear();
        }
        m_work_available.notify_all();
        for (std::thread& worker : m_workers) {
            worker.join();
        }
    }

    [[nodiscard]]
    bool canAccept() const
    {
        std::lock_guard lock{m_mutex};
        return outstandingCount() < MAX_OUTSTANDING_WORK;
    }

    [[nodiscard]]
    bool enqueue(Work work)
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_stopping || outstandingCount() >= MAX_OUTSTANDING_WORK) {
                return false;
            }
            m_work.push_back(std::move(work));
        }
        m_work_available.notify_one();
        return true;
    }

    void cancel(core::ClientId const client_id, uint64_t const generation)
    {
        std::lock_guard lock{m_mutex};
        std::erase_if(m_work, [client_id, generation](Work const& work) {
            return work.client_id == client_id && work.generation == generation;
        });
        std::erase_if(m_results, [client_id, generation](Result const& result) {
            return result.client_id == client_id && result.generation == generation;
        });
        m_result_space_available.notify_all();
    }

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueued(
        core::ClientId const client_id,
        uint64_t const generation
    ) {
        std::vector<shared::GenerationJob> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_work, [&](Work const& work) {
            if (work.client_id != client_id || work.generation != generation) {
                return false;
            }
            cancelled.push_back(work.job);
            return true;
        });
        if (!cancelled.empty()) {
            m_result_space_available.notify_all();
        }
        return cancelled;
    }

    [[nodiscard]]
    std::vector<shared::GenerationJob> cancelQueuedIf(
        core::ClientId const client_id,
        std::function<bool(Work const&)> const& should_cancel
    ) {
        std::vector<shared::GenerationJob> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_work, [&](Work const& work) {
            if (work.client_id != client_id || !should_cancel(work)) {
                return false;
            }
            cancelled.push_back(work.job);
            return true;
        });
        if (!cancelled.empty()) {
            m_result_space_available.notify_all();
        }
        return cancelled;
    }

    void reorderQueued(
        core::ClientId const client_id,
        std::function<bool(Work const&, Work const&)> const& order
    ) {
        std::lock_guard lock{m_mutex};
        std::stable_sort(m_work.begin(), m_work.end(), [client_id, &order](Work const& first, Work const& second) {
            if (first.client_id != client_id || second.client_id != client_id) {
                return first.client_id < second.client_id;
            }
            return order(first, second);
        });
    }

    [[nodiscard]]
    std::vector<Result> takeResults(uint32_t const maximum_results)
    {
        std::vector<Result> results;
        {
            std::lock_guard lock{m_mutex};
            results.reserve(std::min<uint32_t>(maximum_results, static_cast<uint32_t>(m_results.size())));
            while (!m_results.empty() && results.size() < maximum_results) {
                results.push_back(std::move(m_results.front()));
                m_results.pop_front();
            }
        }
        m_result_space_available.notify_all();
        return results;
    }

private:
    [[nodiscard]]
    uint32_t outstandingCount() const noexcept
    {
        return static_cast<uint32_t>(m_work.size() + m_active_workers + m_results.size());
    }

    void workerLoop()
    {
        shared::TerrainGenerator terrain_generator;
        while (true) {
            Work work{};
            {
                std::unique_lock lock{m_mutex};
                m_work_available.wait(lock, [this] {
                    return m_stopping || !m_work.empty();
                });
                if (m_stopping) {
                    return;
                }
                work = std::move(m_work.front());
                m_work.pop_front();
                ++m_active_workers;
            }

            Result result{
                .client_id = work.client_id,
                .generation = work.generation,
                .job = work.job,
            };
            try {
                shared::HeightTileCoordinate const coordinate{
                    .x = work.job.coordinate.x,
                    .y = work.job.coordinate.y,
                };
                result.tile = terrain_generator.generateHeightTile(coordinate);
                result.succeeded = true;
            } catch (...) {
                result.succeeded = false;
            }

            {
                std::unique_lock lock{m_mutex};
                --m_active_workers;
                m_result_space_available.wait(lock, [this] {
                    return m_stopping || outstandingCount() < MAX_OUTSTANDING_WORK;
                });
                if (m_stopping) {
                    return;
                }
                m_results.push_back(std::move(result));
            }
        }
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_work_available;
    std::condition_variable m_result_space_available;
    std::deque<Work> m_work;
    std::deque<Result> m_results;
    std::vector<std::thread> m_workers;
    uint32_t m_active_workers = 0U;
    bool m_stopping = false;
};

GameServer::GameServer(
    uint16_t const port,
    std::vector<SpawnPoint> spawn_points,
    shared::WorldMode const world_mode,
    shared::WorldConfiguration const configuration
)
    : core::Server{core::Address::localhost(port), 4, 2}
    , m_world{world_mode, configuration}
    , m_spawn_points{checkedSpawnPoints(std::move(spawn_points), world_mode)}
{
    shared::prepareHeightTileInterestOrders();
    m_height_tile_workers = std::make_unique<HeightTileWorkerPool>();
}

GameServer::~GameServer() = default;

void GameServer::run()
{
    std::atomic_bool const never_stop{ false };
    run(never_stop);
}

void GameServer::run(std::atomic_bool const& stop_requested)
{
    auto next_simulation = std::chrono::steady_clock::now();
    while (!stop_requested.load(std::memory_order_relaxed)) {
        auto const now = std::chrono::steady_clock::now();
        if (now >= next_simulation) {
            static_cast<void>(tick(std::chrono::milliseconds::zero()));
            next_simulation += shared::TICK;
        } else {
            while (poll(std::chrono::milliseconds::zero()) > 0) {
            }
            processHeightTileStreams(false);
        }
        std::this_thread::sleep_until(std::min(
            next_simulation,
            std::chrono::steady_clock::now() + std::chrono::milliseconds{5}
        ));
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
    for (PlayerReplication& replication : m_player_replications) {
        replication.action_consumed_this_tick = false;
    }
    uint64_t events = static_cast<uint64_t>(poll(timeout));
    while (true) {
        uint32_t const drained = poll(std::chrono::milliseconds::zero());
        if (drained == 0U) {
            break;
        }
        events += static_cast<uint64_t>(drained);
    }
    for (PlayerReplication& replication : m_player_replications) {
        if (!replication.action_consumed_this_tick && !replication.pending_inputs.empty()) {
            shared::ClientInputMessage const input = replication.pending_inputs.front();
            replication.pending_inputs.pop_front();
            processInput(replication, input);
        }
    }
    processHeightTileStreams(true);
    return events;
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
                || spawn_point.z > shared::World::FLIGHT_MAX_Z);
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
    std::erase_if(m_preview_streams, [this, &client](PreviewStream& stream) {
        if (stream.client_id != client.client_id) {
            return false;
        }
        m_height_tile_workers->cancel(stream.client_id, stream.generation);
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
}

void GameServer::onReceived(core::ServerReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        CORE_ERROR("Received a corrupted message");
        return;
    }

    shared::PlayerId const id = event.client_id;
    shared::Message* msg_ptr = &*maybe_msg;
    if (auto* msg = std::get_if<shared::JoinRequestMessage>(msg_ptr)) {
        if (msg->mode != m_world.mode() || msg->configuration != m_world.configuration()) {
            sendTo(id, shared::JoinResponseMessage{
                .accepted = false,
            });
            return;
        }
        char const ch = msg->ch;
        if (m_world.player(id) || m_world.playerExists(ch)) {
            sendTo(id, shared::JoinResponseMessage{
                .accepted = false,
            });
            return;
        }
        auto const spawn_point = std::ranges::find(
            m_spawn_points,
            ch,
            &SpawnPoint::character
        );
        if (spawn_point == m_spawn_points.end()) {
            m_world.spawnPlayer(id, ch);
        } else {
            m_world.spawnPlayer(id, ch, shared::PlayerPosition{
                .x = spawn_point->x,
                .y = spawn_point->y,
                .z = spawn_point->z,
            });
        }
        m_player_replications.push_back(PlayerReplication{ .id = id });
        sendTo(id, shared::JoinResponseMessage{
            .accepted = true,
        });

        shared::Player const p = m_world.player(id).value();
        CORE_INFO("Player '{}' spawned at x {}, y {}, z {}", ch, p.x, p.y, p.z);
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
        if (msg->wants_previews) {
            startHeightTileStream(id);
        }
    } else if (auto* msg = std::get_if<shared::ClientInputMessage>(msg_ptr)) {
        auto const player = m_world.player(id);
        PlayerReplication* const replication = playerReplication(id);
        if (!player || replication == nullptr) {
            return;
        }
        if (replication->has_received_sequence
            && !shared::isNewerSequence(msg->sequence, replication->latest_received_sequence)) {
            return;
        }
        if ((replication->action_consumed_this_tick || !replication->pending_inputs.empty())
            && replication->pending_inputs.size() == PlayerReplication::MAX_PENDING_INPUTS) {
            return;
        }
        replication->latest_received_sequence = msg->sequence;
        replication->has_received_sequence = true;
        if (!replication->action_consumed_this_tick && replication->pending_inputs.empty()) {
            processInput(*replication, *msg);
        } else {
            replication->pending_inputs.push_back(*msg);
        }
    } else if (auto* msg = std::get_if<shared::ClientHeightTileCreditMessage>(msg_ptr)) {
        acknowledgeHeightTileDelivery(id, *msg);
    } else {
        CORE_ERROR("Received a message unsupported by the server {}", msg_ptr->index());
    }
}

void GameServer::processInput(PlayerReplication& replication, shared::ClientInputMessage const input)
{
    replication.action_consumed_this_tick = true;
    bool moved = false;
    if (input.direction.x != 0 || input.direction.y != 0 || input.direction.z != 0) {
        moved = m_world.movePlayer(replication.id, input.direction);
    }
    replication.acknowledged_input_sequence = input.sequence;
    auto const stream = std::ranges::find(m_preview_streams, replication.id, &PreviewStream::client_id);
    if (stream != m_preview_streams.end()) {
        int8_t const movement_x = static_cast<int8_t>(input.direction.x);
        int8_t const movement_y = static_cast<int8_t>(input.direction.y);
        stream->heading_x = movement_x != 0 || movement_y != 0 ? movement_x : input.direction.view_x;
        stream->heading_y = movement_x != 0 || movement_y != 0 ? movement_y : input.direction.view_y;
    }
    ++replication.state_revision;
    shared::ServerPlayerPositionMessage const position = playerPositionMessage(
        *m_world.player(replication.id),
        replication
    );
    if (moved) {
        send(position);
    } else {
        sendTo(replication.id, position);
    }
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
        .x = player.x,
        .y = player.y,
        .z = player.z,
        .x_subcell = player.x_subcell,
        .y_subcell = player.y_subcell,
        .z_subcell = player.z_subcell,
        .acknowledged_input_sequence = replication.acknowledged_input_sequence,
        .state_revision = replication.state_revision,
    };
}

void GameServer::send(shared::Message const message) {
    sendTo(std::nullopt, std::move(message));
}

void GameServer::sendTo(std::optional<core::ClientId> const client_id, shared::Message message) {
    std::vector const message_bytes = shared::encodeMessage(std::move(message));
    std::optional<core::Peer> peer;
    if (client_id.has_value()) {
        peer = client(*client_id);
        if (!peer) {
            CORE_ERROR("Cannot send a message to disconnected client {}", *client_id);
            return;
        }
    }
    if (!core::Server::send(peer, message_bytes, shared::GAME_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a message");
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
        .world_revision = PreviewStream::WORLD_REVISION,
        .max_height_tiles = shared::HEIGHT_TILE_INTEREST_COUNT,
        .max_height_tile_bytes = shared::HEIGHT_TILE_PAYLOAD_BYTES,
    });
    sendHeightTileTo(client_id, shared::ServerWorldRevisionMessage{ .world_revision = PreviewStream::WORLD_REVISION });
    m_preview_streams.emplace_back(client_id);
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
    if (credit.delivery_token == 0U) {
        if (credit.credits != shared::HEIGHT_TILE_DELIVERY_WINDOW
            || stream->delivery_credits != 0U || !stream->inflight_deliveries.empty()) {
            return;
        }
        stream->delivery_credits = credit.credits;
        return;
    }
    if (credit.credits != 1U) {
        return;
    }
    auto const delivery = stream->inflight_deliveries.find(credit.delivery_token);
    if (delivery == stream->inflight_deliveries.end()) {
        return;
    }
    for (shared::HeightTileKey const key : delivery->second.additions) {
        stream->inflight_addition_keys.erase(key);
        stream->resident_keys.insert(key);
    }
    for (shared::HeightTileKey const key : delivery->second.removals) {
        stream->inflight_removal_keys.erase(key);
        stream->pending_removals.erase(key);
        stream->resident_keys.erase(key);
    }
    stream->inflight_deliveries.erase(delivery);
    if (stream->delivery_credits < shared::HEIGHT_TILE_DELIVERY_WINDOW) {
        ++stream->delivery_credits;
    }
    queueDepartedResidentTiles(*stream);
}

void GameServer::admitHeightTileDeliveries(PreviewStream& stream)
{
    static constexpr uint32_t MAX_ADMITTED_BATCHES_PER_PUMP = shared::HEIGHT_TILE_DELIVERY_WINDOW;
    uint32_t const available_batches = std::min<uint32_t>({
        stream.delivery_credits,
        static_cast<uint32_t>(shared::HEIGHT_TILE_DELIVERY_WINDOW - stream.inflight_deliveries.size()),
        MAX_ADMITTED_BATCHES_PER_PUMP,
    });
    uint32_t const maximum_operations = available_batches * shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY;
    if (maximum_operations == 0U) {
        return;
    }

    std::vector<shared::HeightTileKey> ready_keys;
    ready_keys.reserve(stream.ready_tiles.size());
    for (auto const& [key, tile] : stream.ready_tiles) {
        static_cast<void>(tile);
        if (stream.desired_key_set.contains(key) && !stream.inflight_addition_keys.contains(key)) {
            ready_keys.push_back(key);
        }
    }
    auto const ready_order = [&stream](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return heightTilePriority(stream.center, stream.heading_x, stream.heading_y, first)
            < heightTilePriority(stream.center, stream.heading_x, stream.heading_y, second);
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

    uint32_t const maximum_removals = maximum_operations - static_cast<uint32_t>(ready_keys.size());
    std::vector<shared::HeightTileKey> const pending_removals{
        stream.pending_removals.begin(), stream.pending_removals.end()
    };
    std::vector<shared::HeightTileKey> const inflight_removals{
        stream.inflight_removal_keys.begin(), stream.inflight_removal_keys.end()
    };
    std::vector<shared::HeightTileKey> const removal_keys = shared::selectHeightTileRemovalCandidates(
        stream.center,
        stream.heading_x,
        stream.heading_y,
        pending_removals,
        inflight_removals,
        maximum_removals
    );

    auto ready = ready_keys.begin();
    auto removal = removal_keys.begin();
    uint32_t admitted_batches = 0U;
    while (stream.delivery_credits > 0U
        && stream.inflight_deliveries.size() < shared::HEIGHT_TILE_DELIVERY_WINDOW
        && admitted_batches < MAX_ADMITTED_BATCHES_PER_PUMP) {
        shared::ServerHeightTileBatchMessage batch{
            .delivery_token = nextHeightTileToken(m_next_height_tile_token),
        };
        PreviewStream::Delivery delivery;
        batch.tiles.reserve(shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY);
        batch.removals.reserve(shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY);
        while (ready != ready_keys.end()
            && batch.tiles.size() + batch.removals.size() < shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY) {
            shared::HeightTileKey const key = *ready;
            ++ready;
            auto const tile = stream.ready_tiles.find(key);
            if (tile == stream.ready_tiles.end()) {
                continue;
            }
            delivery.additions.push_back(key);
            stream.inflight_addition_keys.insert(key);
            batch.tiles.push_back(std::move(tile->second));
            stream.ready_tiles.erase(tile);
        }
        while (removal != removal_keys.end()
            && batch.tiles.size() + batch.removals.size() < shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY) {
            shared::HeightTileKey const key = *removal;
            ++removal;
            delivery.removals.push_back(key);
            stream.inflight_removal_keys.insert(key);
            batch.removals.push_back({
                .key = key,
                .revision = PreviewStream::WORLD_REVISION,
                .token = nextHeightTileToken(m_next_height_tile_token),
            });
        }
        if (delivery.additions.empty() && delivery.removals.empty()) {
            return;
        }
        uint64_t const delivery_token = batch.delivery_token;
        stream.inflight_deliveries.emplace(delivery_token, std::move(delivery));
        --stream.delivery_credits;
        sendHeightTileTo(stream.client_id, std::move(batch));
        ++admitted_batches;
    }
}

void GameServer::refreshHeightTileInterest(PreviewStream& stream, shared::Player const& player)
{
    shared::HeightTileKey const next_center = heightTileKeyForPlayer(player);
    shared::HeightTileHeading const heading = shared::canonicalHeightTileHeading(
        stream.heading_x, stream.heading_y
    );
    if (stream.has_center && stream.center == next_center
        && stream.applied_heading_x == heading.x
        && stream.applied_heading_y == heading.y) {
        return;
    }
    shared::HeightTileInterest const next_interest = shared::makeHeightTileInterest(
        next_center, heading.x, heading.y
    );

    bool const center_changed = !stream.has_center || stream.center != next_center;
    stream.center = next_center;
    stream.has_center = true;
    stream.applied_heading_x = heading.x;
    stream.applied_heading_y = heading.y;
    stream.desired_keys = next_interest.keys;
    stream.desired_key_set.clear();
    stream.desired_key_set.reserve(stream.desired_keys.size());
    stream.desired_key_set.insert(stream.desired_keys.begin(), stream.desired_keys.end());
    if (center_changed) {
        auto const now = std::chrono::steady_clock::now();
        stream.background_generation_tokens = 0.0;
        stream.background_budget_updated_at = now;
        stream.background_generation_eligible_at = now + std::chrono::milliseconds{250};
    }

    std::erase_if(stream.queued_keys, [&stream](shared::HeightTileKey const key) {
        return !stream.desired_key_set.contains(key);
    });
    stream.scheduler.cancelQueuedIf([&stream](shared::GenerationJob const& job) {
        return !stream.desired_key_set.contains({.x = job.coordinate.x, .y = job.coordinate.y});
    });
    for (shared::GenerationJob const& job : m_height_tile_workers->cancelQueuedIf(
             stream.client_id,
             [&stream](HeightTileWorkerPool::Work const& work) {
                 return !stream.desired_key_set.contains({
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
            stream.heading_x,
            stream.heading_y,
            {.x = first.coordinate.x, .y = first.coordinate.y}
        ) < heightTilePriority(
            stream.center,
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

    std::erase_if(stream.ready_tiles, [&stream](auto const& entry) {
        return !stream.desired_key_set.contains(entry.first);
    });
    queueDepartedResidentTiles(stream);
    fillHeightTileQueue(stream);
}

void GameServer::queueDepartedResidentTiles(PreviewStream& stream)
{
    for (shared::HeightTileKey const key : stream.resident_keys) {
        if (!stream.desired_key_set.contains(key) && !stream.inflight_removal_keys.contains(key)) {
            stream.pending_removals.insert(key);
        }
    }
}

void GameServer::fillHeightTileQueue(PreviewStream& stream)
{
    if (stream.scheduler.pendingCount() >= PreviewStream::MAX_QUEUED_TILES) {
        return;
    }
    // desired_keys is already nearest-first and direction-aware. Walk it directly;
    // rebuilding and sorting thousands of candidates every terrain pump starves the
    // fixed simulation cadence while the initial window is filling.
    for (shared::HeightTileKey const key : stream.desired_keys) {
        if (!stream.resident_keys.contains(key)
            && !stream.queued_keys.contains(key)
            && !stream.dispatched_keys.contains(key)
            && !stream.ready_tiles.contains(key)
            && !stream.inflight_addition_keys.contains(key)) {
            shared::GenerationAdmission const admission = stream.scheduler.submit(
                {.x = key.x, .y = key.y, .z = 0},
                stream.generation,
                shared::GenerationStage::HeightTile
            );
            if (admission == shared::GenerationAdmission::QueueFull) {
                break;
            }
            if (admission == shared::GenerationAdmission::Accepted) {
                stream.queued_keys.insert(key);
            }
        }
    }
}

void GameServer::processHeightTileStreams(bool const admit_deliveries)
{
    for (PreviewStream& stream : m_preview_streams) {
        auto const player = m_world.player(stream.client_id);
        if (player) {
            refreshHeightTileInterest(stream, *player);
        }
    }

    publishHeightTileResults();
    for (PreviewStream& stream : m_preview_streams) {
        fillHeightTileQueue(stream);
        if (admit_deliveries) {
            admitHeightTileDeliveries(stream);
        }
    }
    dispatchHeightTileWork();
}

void GameServer::dispatchHeightTileWork()
{
    static constexpr uint32_t MAX_DISPATCHED_TILES_PER_TICK = PreviewStream::MAX_QUEUED_TILES;
    static constexpr double BACKGROUND_TILES_PER_SECOND = 256.0;
    static constexpr double MAXIMUM_BACKGROUND_BURST = 24.0;
    uint32_t dispatched = 0U;
    for (PreviewStream& stream : m_preview_streams) {
        auto const now = std::chrono::steady_clock::now();
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
        while (dispatched < MAX_DISPATCHED_TILES_PER_TICK && m_height_tile_workers->canAccept()) {
            auto const next = stream.scheduler.peekNext();
            if (!next.has_value()) {
                break;
            }
            bool const background = shared::heightTileGenerationBand(
                stream.center,
                stream.heading_x,
                stream.heading_y,
                {.x = next->coordinate.x, .y = next->coordinate.y}
            ) == shared::HeightTileGenerationBand::Background;
            if (background && stream.background_generation_tokens < 1.0) {
                break;
            }
            auto const job = stream.scheduler.takeNext();
            if (!job) {
                break;
            }
            shared::HeightTileKey const key{.x = job->coordinate.x, .y = job->coordinate.y};
            stream.queued_keys.erase(key);
            if (!m_height_tile_workers->enqueue({
                .client_id = stream.client_id,
                .generation = stream.generation,
                .job = *job,
            })) {
                static_cast<void>(stream.scheduler.complete(job->id, false, true));
                break;
            }
            stream.dispatched_keys.insert(key);
            if (background) {
                stream.background_generation_tokens -= 1.0;
            }
            ++dispatched;
        }
    }
}

void GameServer::publishHeightTileResults()
{
    static constexpr uint32_t MAX_PUBLISHED_TILES_PER_TICK = 2U * shared::HEIGHT_TILE_BATCH_CAPACITY;
    std::vector<HeightTileWorkerPool::Result> const results = m_height_tile_workers->takeResults(
        MAX_PUBLISHED_TILES_PER_TICK
    );
    std::vector<HeightTileWorkerPool::Result> ordered_results = results;
    std::ranges::stable_sort(ordered_results, [this](
        HeightTileWorkerPool::Result const& first,
        HeightTileWorkerPool::Result const& second
    ) {
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
                stream->heading_x,
                stream->heading_y,
                {.x = job.coordinate.x, .y = job.coordinate.y}
            );
        };
        return priority(first.job) < priority(second.job);
    });
    for (HeightTileWorkerPool::Result const& result : ordered_results) {
        auto const stream = std::ranges::find(m_preview_streams, result.client_id, &PreviewStream::client_id);
        if (stream == m_preview_streams.end()) {
            continue;
        }
        if (stream->generation != result.generation || result.job.revision != result.generation) {
            static_cast<void>(stream->scheduler.complete(result.job.id, false, true));
            continue;
        }
        shared::HeightTileKey const key{.x = result.job.coordinate.x, .y = result.job.coordinate.y};
        stream->dispatched_keys.erase(key);
        if (!stream->scheduler.complete(result.job.id, result.succeeded)) {
            continue;
        }
        auto const scheduler_result = stream->scheduler.takeResult();
        if (!scheduler_result || scheduler_result->job.id != result.job.id) {
            continue;
        }
        if (!scheduler_result->succeeded) {
            static_cast<void>(stream->scheduler.retry(result.job.id));
            continue;
        }
        if (!stream->desired_key_set.contains(key) || stream->resident_keys.contains(key)
            || stream->inflight_addition_keys.contains(key)) {
            continue;
        }
        shared::ServerHeightTileMessage tile{
            .key = key,
            .revision = PreviewStream::WORLD_REVISION,
            .token = nextHeightTileToken(m_next_height_tile_token),
            .heights = result.tile.heights,
        };
        stream->ready_tiles.insert_or_assign(key, std::move(tile));
    }
}

} // namespace server
