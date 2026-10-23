#include <client/BotClient.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>

#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>

#include <shared/world/World.hpp>

#include <generation/Generation.hpp>

#include <core/net/Address.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

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

} // namespace

TEST(BotClientTest, StartsThroughLogicalLoadingGateAndWaitsForServerAcknowledgement)
{
    static constexpr std::chrono::seconds DEADLINE{ 20 };
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    auto preparation = boundedTestPreparation(world_configuration);
    server::GameServer server{
        0U,
        {},
        shared::WorldMode::Flight,
        world_configuration,
        8U,
        nullptr,
        preparation,
    };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&] { server.run(stop_server); } };

    client::BotClient bot{ shared::WorldMode::Flight };
    bool saw_ready_prompt = false;
    bool input_before_start_acknowledgement = false;
    uint64_t sent_inputs = 0U;
    client::GameClientBenchmarkHooks const hooks{
        .deadline = std::chrono::steady_clock::now() + DEADLINE,
        .should_stop = [&] {
            return sent_inputs > 0U
                && bot.worldLoadingStatus().state == client::WorldLoadingState::Playing;
        },
        .is_uncapped_phase = {},
        .input_override = {},
        .start_input_override = {},
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            client::WorldLoadingStatus const status = bot.worldLoadingStatus();
            saw_ready_prompt |= status.ready_prompt_frame_rendered_at.has_value();
            if (sample.input_sent) {
                ++sent_inputs;
                input_before_start_acknowledgement |= !status.start_acknowledged_at.has_value()
                    || status.state != client::WorldLoadingState::Playing;
            }
        },
        .on_authoritative_player = {},
        .on_input_sent = {},
        .on_authoritative_state = {},
    };

    bot.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    client::WorldLoadingStatus const status = bot.worldLoadingStatus();
    EXPECT_TRUE(saw_ready_prompt);
    ASSERT_TRUE(status.key_accepted_at.has_value());
    ASSERT_TRUE(status.start_acknowledged_at.has_value());
    EXPECT_GE(*status.start_acknowledged_at, *status.key_accepted_at);
    EXPECT_EQ(status.state, client::WorldLoadingState::Playing);
    EXPECT_GT(sent_inputs, 0U);
    EXPECT_FALSE(input_before_start_acknowledgement);
}
