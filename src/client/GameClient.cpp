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
        poll(std::chrono::milliseconds::zero());
        if (!m_running || !isConnected()) {
            break;
        }
        render();
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
    if (event.channel_id == shared::PREVIEW_CHANNEL) {
        if (auto* msg = std::get_if<shared::ServerPreviewDescriptorMessage>(msg_ptr)) {
            static_cast<void>(applyPreviewDescriptor(*msg));
        } else if (auto* msg = std::get_if<shared::ServerWorldRevisionMessage>(msg_ptr)) {
            static_cast<void>(applyWorldRevision(*msg));
        } else if (auto* msg = std::get_if<shared::ServerChunkPreviewMessage>(msg_ptr)) {
            static_cast<void>(applyPreview(*msg));
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

bool GameClient::applyPreviewDescriptor(shared::ServerPreviewDescriptorMessage const& message)
{
    if (message.configuration != m_world.configuration() || message.world_revision == 0U
        || message.max_preview_chunks == 0U || message.max_preview_bytes == 0U) {
        return false;
    }
    if (message.world_revision != m_preview_revision.generation) {
        m_preview_revision = { .generation = message.world_revision, .revision = 1 };
        m_preview_residency.advanceRevision(m_preview_revision);
    }
    return true;
}

bool GameClient::applyWorldRevision(shared::ServerWorldRevisionMessage const& message)
{
    if (message.world_revision == 0U || message.world_revision < m_preview_revision.generation) {
        return false;
    }
    if (message.world_revision != m_preview_revision.generation) {
        m_preview_revision = { .generation = message.world_revision, .revision = 1 };
        m_preview_residency.advanceRevision(m_preview_revision);
    }
    return true;
}

bool GameClient::applyPreview(shared::ServerChunkPreviewMessage const& message)
{
    if (message.key != shared::normalizePreviewChunkKey(message.key)
        || message.length != message.bytes.size() || message.length == 0U
        || message.length > shared::PREVIEW_MAX_PAYLOAD_BYTES || message.revision == 0U
        || message.token == 0U || m_preview_revision.generation == 0U) {
        return false;
    }
    PreviewReplacementResult const result = m_preview_residency.accept(
        message.key,
        { .generation = m_preview_revision.generation, .revision = message.revision },
        message.level,
        message.token,
        message.bytes
    );
    return result.replacement == PreviewReplacement::Published;
}

} // namespace client
