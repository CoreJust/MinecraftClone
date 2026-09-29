#pragma once

#include "ClientAudio.hpp"
#include "PlayerPresentation.hpp"
#include "PreviewResidency.hpp"

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <core/net/Client.hpp>

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace client {

struct GameClientLoopSample final {
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;
    std::chrono::nanoseconds network_poll_duration{ 0 };
    std::chrono::nanoseconds height_tile_delivery_duration{ 0 };
    std::chrono::nanoseconds render_duration{ 0 };
    uint64_t network_event_count = 0U;
    uint64_t client_message_payload_bytes_sent = 0U;
    uint64_t client_message_payload_bytes_received = 0U;
    bool input_sent = false;
    bool presentation_succeeded = false;
};

struct GameClientBenchmarkHooks final {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::function<bool()> should_stop;
    std::function<bool()> is_uncapped_phase;
    std::function<std::optional<shared::Direction>(uint64_t)> input_override;
    std::function<void(GameClientLoopSample const&)> on_loop;
    std::function<void(shared::Player const&)> on_authoritative_player;
};

class GameClient : public core::Client {
public:
    static constexpr uint32_t MAX_PENDING_INPUTS = 64;

    explicit GameClient(
        shared::WorldMode const mode = shared::WorldMode::Flat,
        shared::WorldConfiguration const configuration = shared::World::canonicalConfiguration(),
        bool const wants_previews = true,
        std::unique_ptr<ClientAudioOutput> audio_output = {}
    )
        : core::Client{ 2 }
        , m_world{ mode, configuration }
        , m_predicted_world{ mode, configuration }
        , m_height_tile_residency{ { .generation = 1, .revision = 1 }, {} }
        , m_wants_previews{ wants_previews }
        , m_client_audio{ std::move(audio_output) }
    { }

    void run(
        core::Address server_address,
        char ch,
        GameClientBenchmarkHooks const* benchmark_hooks = nullptr
    );
    [[nodiscard]] PreviewResidency const& heightTileResidency() const noexcept
    {
        return m_height_tile_residency;
    }
    [[nodiscard]] PreviewResidency& heightTileResidency() noexcept
    {
        return m_height_tile_residency;
    }
protected:
    virtual shared::Direction input() = 0;
    virtual void render() = 0;
    [[nodiscard]] virtual bool presentationSucceeded() const { return false; }

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
    [[nodiscard]] bool applyHeightTile(shared::ServerHeightTileMessage const& message);
    void applyHeightTileBatch(shared::ServerHeightTileBatchMessage const& message);
    [[nodiscard]] bool applyHeightTileRemoval(shared::ServerRemoveHeightTileMessage const& message);
    void processPendingHeightTileDeliveries();
protected:
    static constexpr std::array<char, 5> FLIGHT_CHARACTERS{ '@', '#', '$', '%', '&' };

    void onDisconnected(core::DisconnectEvent const event) override;
    virtual void onConnectionStateReset() { }
    void resetConnectionState();

private:
    void onReceived(core::ReceiveEvent event) override;
protected:
    shared::World m_world;
    shared::World m_predicted_world;
    PlayerPresentation m_player_presentation;
    PreviewResidency m_height_tile_residency;
    bool m_wants_previews;
    ClientAudio m_client_audio;
    HeightTileRevision m_height_tile_revision{ .generation = 1, .revision = 1 };
    std::deque<shared::ServerHeightTileBatchMessage> m_pending_height_tile_deliveries;
    std::unordered_set<uint64_t> m_pending_height_tile_delivery_tokens;
    uint64_t m_height_tile_credit_revision = 0U;
    std::deque<shared::ClientInputMessage> m_pending_inputs;
    std::unordered_map<char, uint32_t> m_state_revisions;
    shared::PlayerId m_next_id = 0;
    uint32_t m_next_input_sequence = 1;
    char m_local_character = 0;
    bool m_running = true;
    bool m_accepted = false;
    uint32_t m_join_character_index = 0;
    GameClientBenchmarkHooks const* m_benchmark_hooks = nullptr;
    uint64_t m_benchmark_bytes_sent = 0U;
    uint64_t m_benchmark_bytes_received = 0U;
private:
    [[nodiscard]] bool sendJoinRequest();
    [[nodiscard]] bool applyHeightTileDescriptor(shared::ServerHeightTileDescriptorMessage const& message);
    [[nodiscard]] bool applyWorldRevision(shared::ServerWorldRevisionMessage const& message);
    [[nodiscard]] bool grantHeightTileCredit(uint64_t delivery_token, uint8_t credits);
    [[nodiscard]] bool queueHeightTileDelivery(shared::ServerHeightTileBatchMessage message);
    void rebuildPrediction();
    void updatePredictedPresentation(std::chrono::steady_clock::time_point updated_at) noexcept;
};

} // namespace client
