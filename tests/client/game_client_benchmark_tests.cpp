#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>
#include <client/PreviewResidency.hpp>
#include <client/WorldLoading.hpp>

#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <generation/Generation.hpp>

#include <core/net/Address.hpp>
#include <core/net/Event.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class ImmediateAudioOutput final : public client::ClientAudioOutput {
public:
    [[nodiscard]] bool start() noexcept override { return true; }
    void stop() noexcept override {}
    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        return !samples.empty();
    }
};

class BenchmarkClient final : public client::GameClient {
public:
    std::function<void()> on_render;

    explicit BenchmarkClient(shared::WorldMode const mode = shared::WorldMode::Flat)
        : GameClient{
            mode,
            shared::World::canonicalConfiguration(),
            true,
            std::make_unique<ImmediateAudioOutput>(),
        }
    { }

private:
    shared::Direction input() override
    {
        return shared::Direction{ .x = 0U, .y = 0U };
    }

    void render() override
    {
        client::WorldLoadingStatus const status = worldLoadingStatus();
        if (status.state == client::WorldLoadingState::ReadyForStart) {
            markLoadingPromptRendered(std::chrono::steady_clock::now());
        }
        if (on_render) {
            on_render();
        }
    }

    [[nodiscard]] bool presentationSucceeded() const override
    {
        return true;
    }
};

std::shared_ptr<server::WorldPreparation> boundedTestPreparation(
    shared::WorldConfiguration const& world_configuration
)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = world_configuration.seed;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    return std::make_shared<server::WorldPreparation>(configuration, world_configuration.algorithm_version);
}

bool readyPromptPresented(BenchmarkClient const& game_client)
{
    client::WorldLoadingStatus const status = game_client.worldLoadingStatus();
    return status.state == client::WorldLoadingState::ReadyForStart
        && status.ready_prompt_frame_rendered_at.has_value();
}

} // namespace

TEST(GameClientBenchmarkTest, SamplesConnectedProductionLoopsAndStopsAtHookBoundary)
{
    static constexpr std::chrono::seconds DEADLINE{ 15 };
    static constexpr uint64_t TARGET_LOOPS = 150U;
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&] { server.run(stop_server); } };

    BenchmarkClient game_client{ shared::WorldMode::Flight };
    uint64_t completed_loops = 0U;
    uint64_t overridden_inputs = 0U;
    uint64_t authoritative_updates = 0U;
    struct SentInputSample final {
        uint32_t sequence = 0U;
        shared::Direction direction{};
        std::chrono::steady_clock::time_point sent_at{};
        std::chrono::steady_clock::time_point scheduled_at{};
    };
    std::vector<SentInputSample> sent_inputs;
    std::vector<uint32_t> acknowledged_sequences;
    std::vector<uint32_t> authoritative_revisions;
    bool input_before_playing = false;
    bool start_requested = false;
    std::optional<std::chrono::steady_clock::time_point> start_requested_at;
    bool invalid_sample = false;
    bool invalid_sent_input = false;
    bool invalid_authoritative_state = false;
    auto const started_at = std::chrono::steady_clock::now();
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return completed_loops >= TARGET_LOOPS
                && overridden_inputs > 0U
                && game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing;
        },
        .is_uncapped_phase = {},
        .input_override = [&](uint64_t) -> std::optional<shared::Direction> {
            input_before_playing |= game_client.worldLoadingStatus().state != client::WorldLoadingState::Playing;
            ++overridden_inputs;
            return shared::Direction{ .x = 1U, .y = 0U };
        },
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                start_requested_at = std::chrono::steady_clock::now();
                return true;
            }
            return false;
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            if (overridden_inputs > 0U
                && game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing) {
                ++completed_loops;
            }
            invalid_sample |= sample.completed_at < sample.started_at
                || sample.network_poll_duration < std::chrono::nanoseconds::zero()
                || sample.render_duration < std::chrono::nanoseconds::zero()
                || !sample.presentation_succeeded;
        },
        .on_authoritative_player = [&](shared::Player const&) {
            ++authoritative_updates;
        },
        .on_input_sent = [&] (
            shared::ClientInputMessage const& message,
            std::chrono::steady_clock::time_point const sent_at,
            std::chrono::steady_clock::time_point const scheduled_at
        ) {
            sent_inputs.push_back(SentInputSample{
                .sequence = message.sequence,
                .direction = message.direction,
                .sent_at = sent_at,
                .scheduled_at = scheduled_at,
            });
            invalid_sent_input |= sent_at < scheduled_at
                || message.direction.x != 1U
                || message.direction.y != 0U;
        },
        .on_authoritative_state = [&] (
            shared::ServerPlayerPositionMessage const& message,
            shared::Player const& player,
            std::chrono::steady_clock::time_point const observed_at
        ) {
            acknowledged_sequences.push_back(message.acknowledged_input_sequence);
            authoritative_revisions.push_back(message.state_revision);
            invalid_authoritative_state |= observed_at < started_at || player.ch != message.ch;
        },
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_EQ(completed_loops, TARGET_LOOPS);
    EXPECT_GT(overridden_inputs, 0U);
    EXPECT_TRUE(start_requested);
    client::WorldLoadingStatus const loading_status = game_client.worldLoadingStatus();
    ASSERT_TRUE(start_requested_at.has_value());
    ASSERT_TRUE(loading_status.key_accepted_at.has_value());
    EXPECT_GE(*loading_status.key_accepted_at, *start_requested_at);
    EXPECT_FALSE(input_before_playing);
    EXPECT_GT(authoritative_updates, 0U);
    ASSERT_FALSE(sent_inputs.empty());
    EXPECT_FALSE(invalid_sent_input);
    EXPECT_FALSE(invalid_authoritative_state);
    for (uint64_t index = 1U; index < sent_inputs.size(); ++index) {
        EXPECT_GT(sent_inputs[index].sequence, sent_inputs[index - 1U].sequence);
    }
    EXPECT_FALSE(authoritative_revisions.empty());
    for (uint64_t index = 1U; index < authoritative_revisions.size(); ++index) {
        EXPECT_GE(authoritative_revisions[index], authoritative_revisions[index - 1U]);
    }
    for (uint32_t const acknowledged_sequence : acknowledged_sequences) {
        if (acknowledged_sequence == 0U) {
            continue;
        }
        EXPECT_NE(std::find_if(sent_inputs.begin(), sent_inputs.end(), [&](SentInputSample const& sample) {
            return sample.sequence == acknowledged_sequence;
        }), sent_inputs.end());
    }
    EXPECT_FALSE(invalid_sample);
}

TEST(GameClientBenchmarkTest, ConnectedProductionLoopAdoptsTheServerWideRadius)
{
    static constexpr uint32_t RADIUS = 128U;
    static constexpr std::chrono::seconds DEADLINE{15};
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{false};
    std::thread server_thread{[&] { server.run(stop_server); }};
    BenchmarkClient game_client{shared::WorldMode::Flight};
    bool start_requested = false;
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing
                && game_client.heightTileResidency().stats().resident_tiles > 0U;
        },
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();
    EXPECT_EQ(game_client.renderDistance(), RADIUS);
    EXPECT_EQ(game_client.requiredHeightTileCount(), 51'433U);
    EXPECT_GT(game_client.heightTileResidency().stats().resident_tiles, 0U);
}

TEST(GameClientBenchmarkTest, AppliedTerrainDeliveryReachesServerBeforeRenderingCompletes)
{
    static constexpr uint32_t RADIUS = 8U;
    static constexpr std::chrono::seconds DEADLINE{ 15 };
    static constexpr std::chrono::milliseconds ACK_DEADLINE{ 500 };
    static constexpr std::chrono::milliseconds ACK_POLL_INTERVAL{ 1 };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::atomic_uint64_t acknowledged_deliveries{ 0U };
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            acknowledged_deliveries.store(metrics.delivery_credit_samples, std::memory_order_release);
        },
        .before_height_tile_completion = {},
    };
    std::thread server_thread{ [&] { server.run(stop_server, &server_hooks); } };
    BenchmarkClient game_client{ shared::WorldMode::Flight };
    bool blocked_after_delivery = false;
    bool acknowledged_during_render = false;
    bool start_requested = false;
    game_client.on_render = [&] {
        if (game_client.heightTileResidency().stats().resident_tiles == 0U) {
            return;
        }
        blocked_after_delivery = true;
        auto const acknowledgement_deadline = std::chrono::steady_clock::now() + ACK_DEADLINE;
        while (acknowledged_deliveries.load(std::memory_order_acquire) == 0U
            && std::chrono::steady_clock::now() < acknowledgement_deadline) {
            std::this_thread::sleep_for(ACK_POLL_INTERVAL);
        }
        acknowledged_during_render = acknowledged_deliveries.load(std::memory_order_acquire) > 0U;
    };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return blocked_after_delivery && acknowledged_during_render
                && game_client.worldLoadingStatus().state == client::WorldLoadingState::Playing;
        },
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(blocked_after_delivery);
    EXPECT_TRUE(acknowledged_during_render);
}

TEST(GameClientBenchmarkTest, QueuedGameplayInputReachesServerWhilePresentationIsBlocked)
{
    static constexpr uint32_t RADIUS = 8U;
    static constexpr std::chrono::seconds DEADLINE{ 15 };
    static constexpr std::chrono::milliseconds SERVER_RECEIVE_DEADLINE{ 500 };
    struct ServerInputObservation final {
        uint32_t sequence;
        std::chrono::steady_clock::time_point received_at;
    };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        RADIUS,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::atomic_bool server_input_observed{ false };
    std::promise<ServerInputObservation> first_server_input;
    std::future<ServerInputObservation> first_server_input_future = first_server_input.get_future();
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = {},
        .on_preview_buffered = {},
        .on_preview_metrics = [&](core::ClientId, server::GameServer::PreviewStreamMetrics const metrics) {
            if (metrics.latest_received_input_sequence > 0U && !server_input_observed.load(
                std::memory_order_relaxed
            )) {
                first_server_input.set_value(ServerInputObservation{
                    .sequence = metrics.latest_received_input_sequence,
                    .received_at = std::chrono::steady_clock::now(),
                });
                server_input_observed.store(true, std::memory_order_release);
            }
        },
        .before_height_tile_completion = {},
    };
    std::thread server_thread{ [&] { server.run(stop_server, &server_hooks); } };
    BenchmarkClient game_client{ shared::WorldMode::Flight };
    uint32_t first_sent_input_sequence = 0U;
    bool start_requested = false;
    bool presentation_blocked_after_input = false;
    bool server_received_input_during_render = false;
    std::optional<uint32_t> received_input_sequence_during_render;
    std::optional<std::chrono::steady_clock::time_point> server_received_at;
    std::optional<std::chrono::steady_clock::time_point> render_finished_at;
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return presentation_blocked_after_input
                && server_input_observed.load(std::memory_order_acquire);
        },
        .is_uncapped_phase = {},
        .input_override = [](uint64_t) -> std::optional<shared::Direction> {
            return shared::Direction{ .x = 1U, .y = 0U };
        },
        .start_input_override = [&](uint64_t) {
            if (!start_requested && readyPromptPresented(game_client)) {
                start_requested = true;
                return true;
            }
            return false;
        },
        .on_loop = {},
        .on_authoritative_player = {},
        .on_input_sent = [&](
            shared::ClientInputMessage const& message,
            std::chrono::steady_clock::time_point,
            std::chrono::steady_clock::time_point
        ) {
            if (first_sent_input_sequence == 0U) {
                first_sent_input_sequence = message.sequence;
            }
        },
        .on_authoritative_state = {},
    };
    game_client.on_render = [&] {
        if (first_sent_input_sequence == 0U || presentation_blocked_after_input) {
            return;
        }
        presentation_blocked_after_input = true;
        server_received_input_during_render = first_server_input_future.wait_for(SERVER_RECEIVE_DEADLINE)
            == std::future_status::ready;
        if (server_received_input_during_render) {
            ServerInputObservation const observation = first_server_input_future.get();
            received_input_sequence_during_render = observation.sequence;
            server_received_at = observation.received_at;
        }
        render_finished_at = std::chrono::steady_clock::now();
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(start_requested);
    EXPECT_GT(first_sent_input_sequence, 0U);
    EXPECT_TRUE(presentation_blocked_after_input);
    EXPECT_TRUE(server_received_input_during_render);
    ASSERT_TRUE(received_input_sequence_during_render.has_value());
    EXPECT_EQ(*received_input_sequence_during_render, first_sent_input_sequence);
    ASSERT_TRUE(server_received_at.has_value());
    ASSERT_TRUE(render_finished_at.has_value());
    EXPECT_LT(*server_received_at, *render_finished_at);
}
