#pragma once

#include "PlayerPresentation.hpp"

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <core/net/Client.hpp>

#include <chrono>
#include <deque>
#include <optional>
#include <unordered_map>

namespace client {

class GameClient : public core::Client {
public:
    static constexpr uint32_t MAX_PENDING_INPUTS = 64;

    explicit GameClient(
        shared::WorldMode const mode = shared::WorldMode::Flat,
        shared::WorldConfiguration const configuration = shared::World::canonicalConfiguration()
    )
        : core::Client{ 1 }
        , m_world{ mode, configuration }
        , m_predicted_world{ mode, configuration }
    { }

    void run(core::Address const server_address, char const ch);
protected:
    virtual shared::Direction input() = 0;
    virtual void render() = 0;

    [[nodiscard]] bool send(shared::Message const message);
    [[nodiscard]]
    std::optional<shared::ClientInputMessage> predictInput(
        shared::Direction direction,
        std::chrono::steady_clock::time_point predicted_at = std::chrono::steady_clock::now()
    );
    void discardPredictedInput(
        uint32_t sequence,
        std::chrono::steady_clock::time_point discarded_at = std::chrono::steady_clock::now()
    );
    [[nodiscard]]
    bool applyServerPosition(
        shared::ServerPlayerPositionMessage const& message,
        std::chrono::steady_clock::time_point received_at = std::chrono::steady_clock::now()
    );
    void applyServerRemoval(char character);
    [[nodiscard]] std::optional<shared::Player> predictedLocalPlayer() const noexcept;
    [[nodiscard]]
    std::optional<PlayerPresentationPosition> predictedLocalPresentation(
        std::chrono::steady_clock::time_point now
    ) const noexcept;
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
    void updatePredictedPresentation(std::chrono::steady_clock::time_point updated_at) noexcept;
};

} // namespace client
