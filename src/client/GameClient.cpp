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
    if (!connect(server_address, std::chrono::milliseconds{ 1'000 })) {
        CORE_ERROR("Failed to connect to server {}", server_address);
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }
    
    static_cast<void>(send(shared::JoinRequestMessage {
        .ch = ch,
    }));
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
            if (auto const predicted_input = predictInput(input())) {
                if (!send(*predicted_input)) {
                    discardPredictedInput(predicted_input->sequence);
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
    if (auto* msg = std::get_if<shared::JoinResponseMessage>(msg_ptr)) {
        auto const [accepted] = *msg;
        m_accepted = accepted;
        if (!accepted) {
            m_running = false;
        }
    } else if (auto* msg = std::get_if<shared::ServerPlayerPositionMessage>(msg_ptr)) {
        static_cast<void>(applyServerPosition(*msg));
    } else if (auto* msg = std::get_if<shared::ServerRemovePlayerMessage>(msg_ptr)) {
        applyServerRemoval(msg->ch);
    } else {
        CORE_ERROR("Received a message unsupported by the client {}", msg_ptr->index());
    }
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

std::optional<shared::ClientInputMessage> GameClient::predictInput(shared::Direction const direction)
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
    return input;
}

void GameClient::discardPredictedInput(uint32_t const sequence)
{
    auto const found = std::find_if(m_pending_inputs.begin(), m_pending_inputs.end(), [sequence](
        shared::ClientInputMessage const& input
    ) {
        return input.sequence == sequence;
    });
    if (found != m_pending_inputs.end()) {
        m_pending_inputs.erase(found);
        rebuildPrediction();
    }
}

bool GameClient::applyServerPosition(shared::ServerPlayerPositionMessage const& message)
{
    auto const known_revision = m_state_revisions.find(message.ch);
    if (known_revision != m_state_revisions.end()
        && !shared::isNewerSequence(message.state_revision, known_revision->second)) {
        return false;
    }
    m_state_revisions.insert_or_assign(message.ch, message.state_revision);
    if (auto const player = m_world.playerByCharacter(message.ch)) {
        m_world.setPlayerPosition(player->id, message.x, message.y, message.x_subcell, message.y_subcell);
    } else {
        shared::PlayerId const id = m_next_id++;
        m_world.spawnPlayer(id, message.ch, {{message.x, message.y}});
        m_world.setPlayerPosition(id, message.x, message.y, message.x_subcell, message.y_subcell);
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
    } else {
        rebuildPrediction();
        if (auto const player = m_world.playerByCharacter(message.ch)) {
            m_player_presentation.update(*player, std::chrono::steady_clock::now());
        }
    }
    return true;
}

std::optional<shared::Player> GameClient::predictedLocalPlayer() const noexcept
{
    return m_predicted_world.playerByCharacter(m_local_character);
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

bool GameClient::send(shared::Message const message)
{
    std::vector const message_bytes = shared::encodeMessage(message);
    if (!core::Client::send(message_bytes, 0, core::SendMode{ core::SendMode::Reliable })) {
        CORE_ERROR("Failed to send a message");
        return false;
    }
    return true;
}

} // namespace client
