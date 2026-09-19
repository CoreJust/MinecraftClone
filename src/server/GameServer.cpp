#include <server/GameServer.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/IO/Log.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {

std::vector<uint8_t> materializePreviewBytes(shared::HeightTile const& tile, int32_t const chunk_z)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(shared::Chunk::BLOCK_COUNT);
    for (uint8_t z = 0U; z < shared::Chunk::SIDE_LENGTH; ++z) {
        uint32_t const world_z = static_cast<uint32_t>(chunk_z) * shared::Chunk::SIDE_LENGTH + z;
        for (uint8_t y = 0U; y < shared::Chunk::SIDE_LENGTH; ++y) {
            for (uint8_t x = 0U; x < shared::Chunk::SIDE_LENGTH; ++x) {
                uint32_t const index = static_cast<uint32_t>(y) * shared::Chunk::SIDE_LENGTH + x;
                bytes.push_back(static_cast<uint8_t>(world_z < tile.heights[index]
                    ? shared::Block::Stone : shared::Block::Air));
            }
        }
    }
    return bytes;
}

} // namespace

namespace server {

void GameServer::run()
{
    std::atomic_bool const never_stop{ false };
    run(never_stop);
}

void GameServer::run(std::atomic_bool const& stop_requested)
{
    while (!stop_requested.load(std::memory_order_relaxed)) {
        auto const start = std::chrono::steady_clock::now();
        static_cast<void>(tick(shared::TICK));
        auto const tick_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        );
        if (auto const delay = fixedTickDelay(tick_time); delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(delay);
        }
    }
}

std::chrono::milliseconds GameServer::fixedTickDelay(std::chrono::milliseconds const elapsed) noexcept
{
    return elapsed < shared::TICK ? shared::TICK - elapsed : std::chrono::milliseconds::zero();
}

uint64_t GameServer::tick(std::chrono::milliseconds const timeout) {
    for (PlayerReplication& replication : m_player_replications) {
        replication.action_consumed_this_tick = false;
        if (!replication.pending_inputs.empty()) {
            shared::ClientInputMessage const input = replication.pending_inputs.front();
            replication.pending_inputs.pop_front();
            processInput(replication, input);
        }
    }
    uint64_t const events = static_cast<uint64_t>(poll(timeout));
    processPendingPreviewSet();
    processPreviewStreams();
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
    std::erase_if(m_pending_preview_sets, [&client](auto const& pending) {
        return pending.first == client.client_id;
    });
    std::erase_if(m_preview_streams, [&client](PreviewStream& stream) {
        if (stream.client_id != client.client_id) {
            return false;
        }
        stream.scheduler.invalidateRevision(PreviewStream::WORLD_REVISION);
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
        if (m_world.mode() == shared::WorldMode::Flight && m_spawn_points.empty() && msg->wants_previews) {
            m_pending_preview_sets.emplace_back(id, p);
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

void GameServer::sendPreviewTo(
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
    if (!core::Server::send(peer, message_bytes, shared::PREVIEW_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a preview message");
    }
}

void GameServer::startPreviewSet(const core::ClientId client_id, shared::Player player)
{
    sendPreviewTo(client_id, shared::ServerPreviewDescriptorMessage{
        .configuration = m_world.configuration(),
        .world_revision = PreviewStream::WORLD_REVISION,
        .max_preview_chunks = PreviewStream::MAX_PREVIEW_CHUNKS,
        .max_preview_bytes = shared::PREVIEW_MAX_PAYLOAD_BYTES,
    });
    sendPreviewTo(client_id, shared::ServerWorldRevisionMessage{ .world_revision = PreviewStream::WORLD_REVISION });

    int32_t const center_x = player.x / shared::Chunk::SIDE_LENGTH;
    int32_t const center_y = player.y / shared::Chunk::SIDE_LENGTH;
    m_preview_streams.emplace_back(client_id, std::move(player));
    PreviewStream& stream = m_preview_streams.back();
    stream.coarse_keys.reserve(PreviewStream::MAX_PREVIEW_CHUNKS);
    int32_t const radius = static_cast<int32_t>(PreviewStream::PREVIEW_RADIUS);
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            shared::PreviewChunkKey const key = shared::normalizePreviewChunkKey({center_x + dx, center_y + dy, 0});
            stream.coarse_keys.push_back(key);
            shared::ChunkCoordinate const coordinate{key.x, key.y, 0};
            static_cast<void>(stream.scheduler.submit(
                coordinate,
                PreviewStream::WORLD_REVISION,
                shared::GenerationStage::HeightTile
            ));
        }
    }
}

void GameServer::processPendingPreviewSet()
{
    if (m_pending_preview_sets.empty()) {
        return;
    }
    auto const [client_id, player] = m_pending_preview_sets.front();
    m_pending_preview_sets.pop_front();
    if (client(client_id).has_value()) {
        startPreviewSet(client_id, std::move(player));
    }
}

void GameServer::processPreviewStreams()
{
    static constexpr uint32_t MAX_JOBS_PER_TICK = 64U;
    uint32_t processed = 0U;
    for (PreviewStream& stream : m_preview_streams) {
        while (processed < MAX_JOBS_PER_TICK) {
            auto const job = stream.scheduler.takeNext();
            if (!job) {
                break;
            }
            try {
                if (job->stage == shared::GenerationStage::HeightTile) {
                    shared::HeightTile tile = m_preview_generator.generateHeightTile({
                        .x = job->coordinate.x,
                        .y = job->coordinate.y,
                    });
                    uint16_t const height = *std::max_element(tile.heights.begin(), tile.heights.end());
                    int32_t const surface_z = static_cast<int32_t>((height - 1U) / shared::Chunk::SIDE_LENGTH);
                    std::vector<uint8_t> bytes = materializePreviewBytes(tile, surface_z);
                    sendPreviewTo(stream.client_id, shared::ServerChunkPreviewMessage{
                        .key = {.x = job->coordinate.x, .y = job->coordinate.y, .z = surface_z},
                        .revision = PreviewStream::WORLD_REVISION,
                        .token = m_next_preview_token++,
                        .level = shared::PreviewLevel::Coarse,
                        .length = static_cast<uint32_t>(bytes.size()),
                        .bytes = std::move(bytes),
                    });
                    ++stream.coarse_completed;
                }
                static_cast<void>(stream.scheduler.complete(job->id, true));
                static_cast<void>(stream.scheduler.takeResult());
            } catch (...) {
                static_cast<void>(stream.scheduler.complete(job->id, false));
                auto const result = stream.scheduler.takeResult();
                if (result && !result->succeeded) {
                    static_cast<void>(stream.scheduler.retry(job->id));
                }
            }
            ++stream.sent_chunks;
            ++processed;
        }
    }
}

} // namespace server
