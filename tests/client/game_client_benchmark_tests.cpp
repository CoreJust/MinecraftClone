#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

class BenchmarkClient final : public client::GameClient {
private:
    shared::Direction input() override
    {
        return shared::Direction{ .x = 0U, .y = 0U };
    }

    void render() override
    { }

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
