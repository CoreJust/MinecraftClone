#pragma once

#include "PlayerPresentation.hpp"

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <core/net/Client.hpp>

#include <deque>
#include <optional>
#include <unordered_map>

namespace client {

class GameClient : public core::Client {
public:
    static constexpr uint32_t MAX_PENDING_INPUTS = 64;

    explicit GameClient() : core::Client { 1 } { }

    void run(core::Address const server_address, char const ch);
protected:
    virtual shared::Direction input() = 0;
    virtual void render() = 0;

    [[nodiscard]] bool send(shared::Message const message);
    [[nodiscard]] std::optional<shared::ClientInputMessage> predictInput(shared::Direction direction);
    void discardPredictedInput(uint32_t sequence);
    [[nodiscard]] bool applyServerPosition(shared::ServerPlayerPositionMessage const& message);
    void applyServerRemoval(char character);
    [[nodiscard]] std::optional<shared::Player> predictedLocalPlayer() const noexcept;
private:
    void onDisconnected(core::DisconnectEvent const event) override;
    void onReceived(core::ReceiveEvent event) override;
protected:
    shared::World m_world;
    shared::World m_predicted_world;
    PlayerPresentation m_player_presentation;
    std::deque<shared::ClientInputMessage> m_pending_inputs;
    std::unordered_map<char, uint32_t> m_state_revisions;
    shared::PlayerId m_next_id = 0;
    uint32_t m_next_input_sequence = 1;
    char m_local_character = 0;
    bool m_running = true;
    bool m_accepted = false;
private:
    void rebuildPrediction();
};

} // namespace client
