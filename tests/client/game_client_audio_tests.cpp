#include <client/GameClient.hpp>

#include <server/GameServer.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>

namespace {

struct AudioOutputState final {
    uint32_t start_count = 0U;
    uint32_t play_count = 0U;
    uint32_t stop_count = 0U;
};

class FakeAudioOutput final : public client::ClientAudioOutput {
public:
    explicit FakeAudioOutput(std::shared_ptr<AudioOutputState> state)
        : m_state{ std::move(state) }
    { }

    [[nodiscard]] bool start() noexcept override
    {
        ++m_state->start_count;
        return true;
    }

    void stop() noexcept override
    {
        ++m_state->stop_count;
    }

    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        if (!samples.empty()) {
            ++m_state->play_count;
        }
        return !samples.empty();
    }

private:
    std::shared_ptr<AudioOutputState> m_state;
};

class AudioIntegrationClient final : public client::GameClient {
public:
    explicit AudioIntegrationClient(std::shared_ptr<AudioOutputState> state)
        : GameClient{
            shared::WorldMode::Flat,
            shared::World::canonicalConfiguration(),
            false,
            std::make_unique<FakeAudioOutput>(state),
        }
        , m_audio_state{ std::move(state) }
    { }

private:
    [[nodiscard]] shared::Direction input() override
    {
        return {};
    }

    void render() override
    {
        if (m_audio_state->play_count > 0U) {
            m_running = false;
        }
    }

    std::shared_ptr<AudioOutputState> m_audio_state;
};

} // namespace

TEST(GameClientAudioTest, PlaysJoinCueOnceAfterServerAcceptsClient)
{
    static constexpr std::chrono::seconds CLIENT_DEADLINE{ 3 };
    server::GameServer::SpawnPoint const spawn{
        .character = '@',
        .x = 2U,
        .y = 2U,
    };
    server::GameServer server{ 0, { spawn } };
    std::atomic_bool stop_server{ false };
    std::thread server_thread{ [&server, &stop_server] {
        server.run(stop_server);
    } };

    auto state = std::make_shared<AudioOutputState>();
    AudioIntegrationClient game_client{ state };
    client::GameClientBenchmarkHooks hooks{
        .deadline = std::chrono::steady_clock::now() + CLIENT_DEADLINE,
    };
    game_client.run(core::Address::localhost(server.port()), '@', &hooks);
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();

    EXPECT_EQ(state->start_count, 1U);
    EXPECT_EQ(state->play_count, 1U);
    EXPECT_EQ(state->stop_count, 1U);
}
