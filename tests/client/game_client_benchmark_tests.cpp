#include <client/ClientAudio.hpp>
#include <client/GameClient.hpp>
#include <client/PreviewResidency.hpp>

#include <server/GameServer.hpp>

#include <shared/world/World.hpp>

#include <core/net/Address.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
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
        if (on_render) {
            on_render();
        }
    }

    [[nodiscard]] bool presentationSucceeded() const override
    {
        return true;
    }
};

} // namespace

TEST(GameClientBenchmarkTest, SamplesConnectedProductionLoopsAndStopsAtHookBoundary)
{
    static constexpr std::chrono::seconds DEADLINE{ 3 };
    static constexpr uint64_t TARGET_LOOPS = 150U;
    server::GameServer server{ 0U, {
        server::GameServer::SpawnPoint{ .character = '@', .x = 2, .y = 2 },
    } };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&] { server.run(stop_server); } };

    BenchmarkClient game_client;
    uint64_t completed_loops = 0U;
    uint64_t overridden_inputs = 0U;
    uint64_t authoritative_updates = 0U;
    bool invalid_sample = false;
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] { return completed_loops >= TARGET_LOOPS; },
        .is_uncapped_phase = {},
        .input_override = [&](uint64_t) -> std::optional<shared::Direction> {
            ++overridden_inputs;
            return shared::Direction{ .x = 1U, .y = 0U };
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            ++completed_loops;
            invalid_sample |= sample.completed_at < sample.started_at
                || sample.network_poll_duration < std::chrono::nanoseconds::zero()
                || sample.render_duration < std::chrono::nanoseconds::zero()
                || !sample.presentation_succeeded;
        },
        .on_authoritative_player = [&](shared::Player const&) {
            ++authoritative_updates;
        },
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_EQ(completed_loops, TARGET_LOOPS);
    EXPECT_GT(overridden_inputs, 0U);
    EXPECT_GT(authoritative_updates, 0U);
    EXPECT_FALSE(invalid_sample);
}

TEST(GameClientBenchmarkTest, ConnectedProductionLoopAdoptsTheServerWideRadius)
{
    static constexpr uint32_t RADIUS = 128U;
    static constexpr std::chrono::seconds DEADLINE{5};
    server::GameServer server{
        0U, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(), RADIUS,
    };
    std::atomic_bool stop_server{false};
    std::thread server_thread{[&] { server.run(stop_server); }};
    BenchmarkClient game_client{shared::WorldMode::Flight};
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] { return game_client.heightTileResidency().stats().resident_tiles > 0U; },
        .is_uncapped_phase = {},
        .input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
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
    static constexpr std::chrono::seconds DEADLINE{ 5 };
    static constexpr std::chrono::milliseconds ACK_DEADLINE{ 500 };
    static constexpr std::chrono::milliseconds ACK_POLL_INTERVAL{ 1 };
    server::GameServer server{
        0U, {}, shared::WorldMode::Flight, shared::World::canonicalConfiguration(), RADIUS,
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
        .should_stop = [&] { return blocked_after_delivery; },
        .is_uncapped_phase = {},
        .input_override = {},
        .on_loop = {},
        .on_authoritative_player = {},
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_TRUE(blocked_after_delivery);
    EXPECT_TRUE(acknowledged_during_render);
}
