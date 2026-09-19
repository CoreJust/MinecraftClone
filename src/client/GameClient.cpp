#include <client/GameClient.hpp>

#include <client/FrameScheduler.hpp>

#include <core/common/SpanUtils.hpp>
#include <core/IO/Log.hpp>

#include <algorithm>
#include <iostream>
#include <optional>

namespace client {

void GameClient::run(core::Address const server_address, char const ch) {
    m_local_character = ch;
    m_running = true;
    m_accepted = false;
    m_join_character_index = 0;
    if (m_world.mode() == shared::WorldMode::Flight) {
        auto const character = std::ranges::find(FLIGHT_CHARACTERS, ch);
        if (character != FLIGHT_CHARACTERS.end()) {
            m_join_character_index = static_cast<uint32_t>(character - FLIGHT_CHARACTERS.begin());
        }
    }
    if (!connect(server_address, std::chrono::milliseconds{ 1'000 })) {
        CORE_ERROR("Failed to connect to server {}", server_address);
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }
    
    if (!sendJoinRequest()) {
        m_running = false;
    }
    while (!m_accepted && m_running && isConnected()) {
        poll(std::chrono::milliseconds{ 100 });
    }

    FrameScheduler scheduler{ std::chrono::steady_clock::now(), shared::TICK };
    while (m_running && isConnected()) {
        while (poll(std::chrono::milliseconds::zero()) > 0) {
        }
        processPendingHeightTileDeliveries();
        if (!m_running || !isConnected()) {
            break;
        }
        std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
        if (scheduler.simulationDue(now)) {
            if (auto const predicted_input = predictInput(input(), now)) {
                if (!send(*predicted_input)) {
                    discardPredictedInput(predicted_input->sequence, now);
                }
            }
        }
        render();
        if (!m_running || !isConnected()) {
            break;
        }
        std::this_thread::sleep_for(scheduler.idleDelay(now));
    }
}

void GameClient::onDisconnected(core::DisconnectEvent const event) {
    CORE_INFO("Server disconnected: {}", event.peer.address());
    std::cout << "[SERVER DISCONNECTED] address " << fmt::format("{}", event.peer.address()) << std::endl;
    m_running = false;
}

void GameClient::onReceived(core::ReceiveEvent event) {
    std::optional maybe_msg = shared::decodeMessage(event.data);
    if (!maybe_msg) {
        CORE_ERROR("Received a corrupted message");
        return;
    }

    shared::Message* msg_ptr = &*maybe_msg;
    if (event.channel_id == shared::HEIGHT_TILE_CHANNEL) {
        if (auto* msg = std::get_if<shared::ServerHeightTileDescriptorMessage>(msg_ptr)) {
            static_cast<void>(applyHeightTileDescriptor(*msg));
        } else if (auto* msg = std::get_if<shared::ServerWorldRevisionMessage>(msg_ptr)) {
            static_cast<void>(applyWorldRevision(*msg));
        } else if (auto* msg = std::get_if<shared::ServerHeightTileMessage>(msg_ptr)) {
            static_cast<void>(applyHeightTile(*msg));
        } else if (auto* msg = std::get_if<shared::ServerHeightTileBatchMessage>(msg_ptr)) {
            static_cast<void>(queueHeightTileDelivery(std::move(*msg)));
        } else if (auto* msg = std::get_if<shared::ServerRemoveHeightTileMessage>(msg_ptr)) {
            static_cast<void>(applyHeightTileRemoval(*msg));
        }
    } else if (event.channel_id != shared::GAME_CHANNEL) {
        CORE_ERROR("Received a game message on an unsupported channel {}", event.channel_id);
    } else if (auto* msg = std::get_if<shared::JoinResponseMessage>(msg_ptr)) {
        auto const [accepted] = *msg;
        if (accepted) {
            m_accepted = true;
        } else if (m_accepted) {
            m_running = false;
        } else {
            if (m_world.mode() != shared::WorldMode::Flight
                || m_join_character_index + 1U >= FLIGHT_CHARACTERS.size()) {
                m_running = false;
            } else {
                ++m_join_character_index;
                m_local_character = FLIGHT_CHARACTERS[m_join_character_index];
                if (!sendJoinRequest()) {
                    m_running = false;
                }
            }
        }
    } else if (auto* msg = std::get_if<shared::ServerPlayerPositionMessage>(msg_ptr)) {
        static_cast<void>(applyServerPosition(*msg));
    } else if (auto* msg = std::get_if<shared::ServerRemovePlayerMessage>(msg_ptr)) {
        applyServerRemoval(msg->ch);
    } else {
        CORE_ERROR("Received a message unsupported by the client {}", msg_ptr->index());
    }
}

bool GameClient::sendJoinRequest()
{
    return send(shared::JoinRequestMessage{
        .ch = m_local_character,
        .mode = m_world.mode(),
        .configuration = m_world.configuration(),
        .wants_previews = m_wants_previews && m_world.mode() == shared::WorldMode::Flight,
    });
}

void GameClient::applyServerRemoval(char const character)
{
    if (auto const player = m_world.playerByCharacter(character)) {
        m_player_presentation.remove(character);
        m_world.despawnPlayer(player->id);
        m_state_revisions.erase(character);
        rebuildPrediction();
    }
}

std::optional<shared::ClientInputMessage> GameClient::predictInput(
    shared::Direction const direction,
    std::chrono::steady_clock::time_point const predicted_at
)
{
    if (!m_predicted_world.playerByCharacter(m_local_character).has_value()
        || m_pending_inputs.size() == MAX_PENDING_INPUTS) {
        return std::nullopt;
    }
    shared::ClientInputMessage const input{
        .direction = direction,
        .sequence = m_next_input_sequence++,
    };
    m_pending_inputs.push_back(input);
    shared::Player const player = *m_predicted_world.playerByCharacter(m_local_character);
    static_cast<void>(m_predicted_world.movePlayer(player.id, direction));
    updatePredictedPresentation(predicted_at);
    return input;
}

void GameClient::discardPredictedInput(
    uint32_t const sequence,
    std::chrono::steady_clock::time_point const discarded_at
)
{
    auto const found = std::find_if(m_pending_inputs.begin(), m_pending_inputs.end(), [sequence](
        shared::ClientInputMessage const& input
    ) {
        return input.sequence == sequence;
    });
    if (found != m_pending_inputs.end()) {
        m_pending_inputs.erase(found);
        rebuildPrediction();
        updatePredictedPresentation(discarded_at);
    }
}

bool GameClient::applyServerPosition(
    shared::ServerPlayerPositionMessage const& message,
    std::chrono::steady_clock::time_point const received_at
)
{
    auto const known_revision = m_state_revisions.find(message.ch);
    if (known_revision != m_state_revisions.end()
        && !shared::isNewerSequence(message.state_revision, known_revision->second)) {
        return false;
    }
    m_state_revisions.insert_or_assign(message.ch, message.state_revision);
    if (auto const player = m_world.playerByCharacter(message.ch)) {
        static_cast<void>(m_world.setPlayerPosition(player->id, {
            .x = message.x,
            .y = message.y,
            .z = message.z,
            .x_subcell = message.x_subcell,
            .y_subcell = message.y_subcell,
            .z_subcell = message.z_subcell,
        }));
    } else {
        shared::PlayerId const id = m_next_id++;
        m_world.spawnPlayer(id, message.ch, {
            .x = message.x,
            .y = message.y,
            .z = message.z,
            .x_subcell = message.x_subcell,
            .y_subcell = message.y_subcell,
            .z_subcell = message.z_subcell,
        });
    }
    if (message.ch == m_local_character) {
        while (!m_pending_inputs.empty()
            && !shared::isNewerSequence(
                m_pending_inputs.front().sequence,
                message.acknowledged_input_sequence
            )) {
            m_pending_inputs.pop_front();
        }
        rebuildPrediction();
        updatePredictedPresentation(received_at);
    } else {
        rebuildPrediction();
        updatePredictedPresentation(received_at);
        if (auto const player = m_world.playerByCharacter(message.ch)) {
            m_player_presentation.update(*player, received_at);
        }
    }
    return true;
}

std::optional<shared::Player> GameClient::predictedLocalPlayer() const noexcept
{
    return m_predicted_world.playerByCharacter(m_local_character);
}

std::optional<PlayerPresentationPosition> GameClient::predictedLocalPresentation(
    std::chrono::steady_clock::time_point const now
) const noexcept
{
    return m_player_presentation.sample(m_local_character, now);
}

void GameClient::rebuildPrediction()
{
    m_predicted_world = m_world;
    if (auto const player = m_predicted_world.playerByCharacter(m_local_character)) {
        for (shared::ClientInputMessage const& input : m_pending_inputs) {
            static_cast<void>(m_predicted_world.movePlayer(player->id, input.direction));
        }
    }
}

void GameClient::updatePredictedPresentation(std::chrono::steady_clock::time_point const updated_at) noexcept
{
    if (auto const player = m_predicted_world.playerByCharacter(m_local_character)) {
        m_player_presentation.update(*player, updated_at);
    }
}

bool GameClient::send(shared::Message const message)
{
    std::vector const message_bytes = shared::encodeMessage(message);
    if (!core::Client::send(message_bytes, shared::GAME_CHANNEL, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a message");
        return false;
    }
    return true;
}

bool GameClient::applyHeightTileDescriptor(shared::ServerHeightTileDescriptorMessage const& message)
{
    if (message.configuration != m_world.configuration() || message.world_revision == 0U
        || message.max_height_tiles < shared::HEIGHT_TILE_INTEREST_COUNT
        || message.max_height_tile_bytes != shared::HEIGHT_TILE_PAYLOAD_BYTES) {
        return false;
    }
    if (message.world_revision != m_height_tile_revision.generation) {
        m_height_tile_revision = {
            .generation = message.world_revision,
            .revision = message.world_revision,
        };
        m_height_tile_residency.advanceRevision(m_height_tile_revision);
        m_pending_height_tile_deliveries.clear();
        m_pending_height_tile_delivery_tokens.clear();
    }
    if (m_height_tile_credit_revision == message.world_revision) {
        return true;
    }
    m_height_tile_credit_revision = message.world_revision;
    return grantHeightTileCredit(0U, shared::HEIGHT_TILE_DELIVERY_WINDOW);
}

bool GameClient::applyWorldRevision(shared::ServerWorldRevisionMessage const& message)
{
    if (message.world_revision == 0U || message.world_revision < m_height_tile_revision.generation) {
        return false;
    }
    if (message.world_revision != m_height_tile_revision.generation) {
        m_height_tile_revision = {
            .generation = message.world_revision,
            .revision = message.world_revision,
        };
        m_height_tile_residency.advanceRevision(m_height_tile_revision);
        m_pending_height_tile_deliveries.clear();
        m_pending_height_tile_delivery_tokens.clear();
    }
    if (m_height_tile_credit_revision == message.world_revision) {
        return true;
    }
    m_height_tile_credit_revision = message.world_revision;
    return grantHeightTileCredit(0U, shared::HEIGHT_TILE_DELIVERY_WINDOW);
}

bool GameClient::grantHeightTileCredit(uint64_t const delivery_token, uint8_t const credits)
{
    return send(shared::ClientHeightTileCreditMessage{
        .world_revision = m_height_tile_revision.generation,
        .delivery_token = delivery_token,
        .credits = credits,
    });
}

bool GameClient::queueHeightTileDelivery(shared::ServerHeightTileBatchMessage message)
{
    if (message.delivery_token == 0U
        || m_pending_height_tile_deliveries.size() >= shared::HEIGHT_TILE_DELIVERY_WINDOW
        || !m_pending_height_tile_delivery_tokens.insert(message.delivery_token).second) {
        return false;
    }
    m_pending_height_tile_deliveries.push_back(std::move(message));
    return true;
}

void GameClient::processPendingHeightTileDeliveries()
{
    uint32_t constexpr MAX_PENDING_HANDOFF_CHANGES = shared::HEIGHT_TILE_BATCH_CAPACITY
        * shared::HEIGHT_TILE_DELIVERY_WINDOW;
    while (!m_pending_height_tile_deliveries.empty()) {
        shared::ServerHeightTileBatchMessage const& delivery = m_pending_height_tile_deliveries.front();
        uint32_t const operation_count = static_cast<uint32_t>(
            delivery.tiles.size() + delivery.removals.size()
        );
        if (m_height_tile_residency.pendingChangeCount() + operation_count > MAX_PENDING_HANDOFF_CHANGES) {
            return;
        }
        for (shared::ServerHeightTileMessage const& tile : delivery.tiles) {
            static_cast<void>(applyHeightTile(tile));
        }
        for (shared::ServerRemoveHeightTileMessage const& removal : delivery.removals) {
            static_cast<void>(applyHeightTileRemoval(removal));
        }
        uint64_t const delivery_token = delivery.delivery_token;
        m_pending_height_tile_delivery_tokens.erase(delivery_token);
        m_pending_height_tile_deliveries.pop_front();
        if (!grantHeightTileCredit(delivery_token, 1U)) {
            m_running = false;
            return;
        }
    }
}

bool GameClient::applyHeightTile(shared::ServerHeightTileMessage const& message)
{
    if (message.key != shared::normalizeHeightTileKey(message.key) || message.revision == 0U
        || message.token == 0U || m_height_tile_revision.generation == 0U) {
        return false;
    }
    HeightTileReplacementResult const result = m_height_tile_residency.accept(
        message.key,
        { .generation = m_height_tile_revision.generation, .revision = message.revision },
        message.token,
        message.heights
    );
    return result.replacement == HeightTileReplacement::Published;
}

void GameClient::applyHeightTileBatch(shared::ServerHeightTileBatchMessage const& message)
{
    for (shared::ServerHeightTileMessage const& tile : message.tiles) {
        static_cast<void>(applyHeightTile(tile));
    }
}

bool GameClient::applyHeightTileRemoval(shared::ServerRemoveHeightTileMessage const& message)
{
    if (message.key != shared::normalizeHeightTileKey(message.key) || message.revision == 0U
        || message.token == 0U || m_height_tile_revision.generation == 0U) {
        return false;
    }
    return m_height_tile_residency.evict(
        message.key,
        { .generation = m_height_tile_revision.generation, .revision = message.revision },
        message.token
    );
}

} // namespace client
