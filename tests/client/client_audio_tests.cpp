#include <client/ClientAudio.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace {

struct FakeOutputState final {
    bool start_result = true;
    uint32_t start_count = 0U;
    uint32_t play_count = 0U;
    uint32_t stop_count = 0U;
    uint32_t played_sample_count = 0U;
};

class FakeOutput final : public client::ClientAudioOutput {
public:
    explicit FakeOutput(std::shared_ptr<FakeOutputState> state)
        : m_state{ std::move(state) }
    { }

    [[nodiscard]] bool start() noexcept override
    {
        ++m_state->start_count;
        return m_state->start_result;
    }

    void stop() noexcept override
    {
        ++m_state->stop_count;
    }

    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        ++m_state->play_count;
        m_state->played_sample_count = static_cast<uint32_t>(samples.size());
        return true;
    }

private:
    std::shared_ptr<FakeOutputState> m_state;
};

} // namespace

TEST(ClientAudioTest, PlaysJoinCueAndStopsOutputOnce)
{
    static constexpr uint32_t EXPECTED_SAMPLE_COUNT = 960U;
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };

    EXPECT_TRUE(audio.start());
    EXPECT_TRUE(audio.start());
    EXPECT_TRUE(audio.playJoinCue());
    audio.stop();
    audio.stop();

    EXPECT_EQ(state->start_count, 1U);
    EXPECT_EQ(state->play_count, 1U);
    EXPECT_EQ(state->played_sample_count, EXPECTED_SAMPLE_COUNT);
    EXPECT_EQ(state->stop_count, 1U);
}

TEST(ClientAudioTest, DeviceStartFailureIsRecoverable)
{
    auto state = std::make_shared<FakeOutputState>();
    state->start_result = false;
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };

    EXPECT_FALSE(audio.start());
    EXPECT_FALSE(audio.playJoinCue());
    state->start_result = true;
    EXPECT_TRUE(audio.start());
    EXPECT_TRUE(audio.playJoinCue());
    audio.stop();

    EXPECT_EQ(state->start_count, 2U);
    EXPECT_EQ(state->play_count, 1U);
    EXPECT_EQ(state->stop_count, 1U);
}

TEST(ClientAudioTest, DestructionStopsStartedOutput)
{
    auto state = std::make_shared<FakeOutputState>();
    {
        client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
        ASSERT_TRUE(audio.start());
    }

    EXPECT_EQ(state->stop_count, 1U);
}
