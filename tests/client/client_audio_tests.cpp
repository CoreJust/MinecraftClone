#include <client/ClientAudio.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

struct FakeOutputState final {
    bool start_result = true;
    bool play_result = true;
    std::vector<std::span<int16_t const>> borrowed;
    std::vector<std::vector<int16_t>> submitted_samples;
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
        // Native stop must finish reading every borrowed clip before returning.
        for (size_t i = 0; i < m_state->borrowed.size(); ++i) {
            auto const samples = m_state->borrowed[i];
            EXPECT_EQ(samples.front(), 0);
            EXPECT_EQ(samples.back(), 0);
            EXPECT_TRUE(std::equal(samples.begin(), samples.end(), m_state->submitted_samples[i].begin()));
        }
    }

    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        ++m_state->play_count;
        m_state->played_sample_count = static_cast<uint32_t>(samples.size());
        m_state->borrowed.push_back(samples);
        m_state->submitted_samples.emplace_back(samples.begin(), samples.end());
        return m_state->play_result;
    }

private:
    std::shared_ptr<FakeOutputState> m_state;
};

std::vector<uint8_t> musicWave(int16_t value)
{
    std::vector<uint8_t> bytes;
    auto text = [&](std::string_view tag) { bytes.insert(bytes.end(), tag.begin(), tag.end()); };
    auto u16 = [&](uint16_t number) {
        bytes.push_back(static_cast<uint8_t>(number));
        bytes.push_back(static_cast<uint8_t>(number >> 8));
    };
    auto u32 = [&](uint32_t number) { u16(static_cast<uint16_t>(number)); u16(static_cast<uint16_t>(number >> 16)); };
    text("RIFF"); u32(48'036); text("WAVE");
    text("fmt "); u32(16); u16(1); u16(1); u32(24'000); u32(48'000); u16(2); u16(16);
    text("data"); u32(48'000);
    for (size_t sample = 0; sample < 24'000; ++sample) {
        u16(static_cast<uint16_t>(sample % 16 < 8 ? value : -value));
    }
    return bytes;
}

} // namespace

TEST(ClientAudioTest, PlaysJoinCueAndStopsOutputOnce)
{
    static constexpr uint32_t EXPECTED_SAMPLE_COUNT = 11'520U;
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

TEST(ClientAudioTest, CreationCadenceHasNoCatchUpBurstAndReadyPlaysOnce)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    audio.updateCreationAudio(client::CreationAudioStage::Molten, 0.0);
    audio.updateCreationAudio(client::CreationAudioStage::Molten, 1.84);
    EXPECT_EQ(state->play_count, 1U);
    audio.updateCreationAudio(client::CreationAudioStage::Molten, 1.85);
    EXPECT_EQ(state->play_count, 2U);
    audio.updateCreationAudio(client::CreationAudioStage::Water, 100.0);
    EXPECT_EQ(state->play_count, 3U);
    audio.updateCreationAudio(client::CreationAudioStage::Ready, 101.0);
    audio.updateCreationAudio(client::CreationAudioStage::Ready, 1000.0);
    EXPECT_EQ(state->play_count, 4U);
    audio.updateCreationAudio(client::CreationAudioStage::Silent, 1001.0);
    audio.updateCreationAudio(client::CreationAudioStage::Silent, 1002.0);
    EXPECT_EQ(state->play_count, 4U);
    EXPECT_EQ(state->stop_count, 0U);
}

TEST(ClientAudioTest, RejectionRetainsImmutableBorrowAndRetriesAtBoundedCadence)
{
    auto state = std::make_shared<FakeOutputState>();
    state->play_result = false;
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    audio.updateCreationAudio(client::CreationAudioStage::Ice, 0.0);
    auto const first = state->borrowed.front();
    audio.updateCreationAudio(client::CreationAudioStage::Ice, 0.24);
    EXPECT_EQ(state->play_count, 1U);
    state->play_result = true;
    audio.updateCreationAudio(client::CreationAudioStage::Ice, 0.25);
    ASSERT_EQ(state->play_count, 2U);
    EXPECT_EQ(first.data(), state->borrowed.back().data());
    EXPECT_EQ(first.front(), 0);
    audio.stop();
    EXPECT_EQ(first.back(), 0);
}

TEST(ClientAudioTest, MaterialPaletteIsDistinctAndChipsDoNotSuppressAcceptedBreaks)
{
    std::vector<int16_t> previous;
    for (uint8_t material = 1; material <= 7; ++material) {
        auto state = std::make_shared<FakeOutputState>();
        client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
        ASSERT_TRUE(audio.start());
        EXPECT_TRUE(audio.playBlockSound(client::BlockSoundKind::Chip, static_cast<shared::Block>(material)));
        EXPECT_FALSE(audio.playBlockSound(client::BlockSoundKind::Chip, static_cast<shared::Block>(material)));
        EXPECT_TRUE(audio.playBlockSound(client::BlockSoundKind::Break, static_cast<shared::Block>(material)));
        auto const samples = state->borrowed.back();
        EXPECT_EQ(samples.size() % 2, 0U);
        EXPECT_LE(*std::max_element(samples.begin(), samples.end()), 3000);
        EXPECT_GE(*std::min_element(samples.begin(), samples.end()), -3000);
        std::vector<int16_t> current(samples.begin(), samples.end());
        EXPECT_NE(current, previous);
        previous = std::move(current);
    }
}

TEST(ClientAudioTest, InvalidInputsAndVoicePressureAreBounded)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    EXPECT_FALSE(audio.playBlockSound(client::BlockSoundKind::Break, shared::Block::Stone));
    ASSERT_TRUE(audio.start());
    EXPECT_FALSE(audio.playBlockSound(client::BlockSoundKind::Break, shared::Block::Air));
    EXPECT_FALSE(audio.playBlockSound(static_cast<client::BlockSoundKind>(255), shared::Block::Stone));
    audio.updateCreationAudio(client::CreationAudioStage::Water, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(state->play_count, 0U);
    for (int count = 0; count < 100; ++count) {
        (void)audio.playJoinCue();
    }
    EXPECT_EQ(state->play_count, 8U);
    audio.stop();
    ASSERT_TRUE(audio.start());
    EXPECT_TRUE(audio.playJoinCue());
}

TEST(ClientAudioTest, EveryCreationStageEmitsDistinctNonSilentFadedStereo)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    for (uint8_t stage = 1; stage <= 6; ++stage) {
        audio.updateCreationAudio(static_cast<client::CreationAudioStage>(stage), stage);
        ASSERT_EQ(state->borrowed.size(), stage);
        auto const samples = state->borrowed.back();
        ASSERT_GT(samples.size(), 48'000U);
        EXPECT_EQ(samples.size() % 2, 0U);
        EXPECT_EQ(samples.front(), 0);
        EXPECT_EQ(samples.back(), 0);
        EXPECT_GT(*std::max_element(samples.begin(), samples.end()), 100);
        if (stage > 1) {
            EXPECT_NE(state->submitted_samples[stage - 1], state->submitted_samples[stage - 2]);
        }
    }
}

TEST(ClientAudioTest, JoinAcknowledgementDoesNotEmitOrScheduleCreationStages)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    ASSERT_TRUE(audio.playJoinCue());
    auto const connection_samples = state->submitted_samples.front();
    audio.updateCreationAudio(client::CreationAudioStage::Silent, 0.0);
    audio.updateCreationAudio(client::CreationAudioStage::Silent, 100.0);
    EXPECT_EQ(state->play_count, 1U);
    EXPECT_EQ(state->stop_count, 0U);
    for (uint8_t stage = 1; stage <= 6; ++stage) {
        audio.updateCreationAudio(static_cast<client::CreationAudioStage>(stage), stage + 100.0);
        ASSERT_EQ(state->play_count, stage + 1U);
        EXPECT_NE(state->submitted_samples.back(), connection_samples);
    }
    // Ready is still admitted by its explicit stage, once rather than by joining.
    audio.updateCreationAudio(client::CreationAudioStage::Ready, 1000.0);
    EXPECT_EQ(state->play_count, 7U);
}

TEST(ClientAudioTest, StoneAndDirtBreaksRetainDifferentRecordedTransientShapes)
{
    std::array<std::vector<int16_t>, 2> impacts;
    constexpr std::array<shared::Block, 2> MATERIALS{shared::Block::Stone, shared::Block::Dirt};
    for (size_t index = 0; index < MATERIALS.size(); ++index) {
        auto state = std::make_shared<FakeOutputState>();
        client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
        ASSERT_TRUE(audio.start());
        ASSERT_TRUE(audio.playBlockSound(client::BlockSoundKind::Break, MATERIALS[index]));
        impacts[index] = state->submitted_samples.back();
    }
    // Different gain applied to the same recording must fail this regression.
    // Compare normalized waveform shape rather than requiring different PCM bytes.
    double dot = 0.0;
    double stone_energy = 0.0;
    double dirt_energy = 0.0;
    auto const count = std::min(impacts[0].size(), impacts[1].size());
    for (size_t sample = 0; sample < count; sample += 2) {
        double const stone = impacts[0][sample];
        double const dirt = impacts[1][sample];
        dot += stone * dirt;
        stone_energy += stone * stone;
        dirt_energy += dirt * dirt;
    }
    ASSERT_GT(stone_energy, 0.0);
    ASSERT_GT(dirt_energy, 0.0);
    EXPECT_LT(std::abs(dot) / std::sqrt(stone_energy * dirt_energy), 0.5);
}

TEST(ClientAudioTest, RecordedWeatherAdvancesThroughFadedSegmentsWithoutFrameBursts)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    for (size_t segment = 0; segment < 4; ++segment) {
        double const time = static_cast<double>(segment) * 1.85;
        audio.updateCreationAudio(client::CreationAudioStage::Water, time);
        ASSERT_EQ(state->submitted_samples.size(), segment + 1);
        auto const& samples = state->submitted_samples.back();
        EXPECT_EQ(samples.size(), 192'000U); // Two seconds at the output's stereo 48 kHz.
        EXPECT_EQ(samples.front(), 0);
        EXPECT_EQ(samples.back(), 0);
        EXPECT_LE(*std::max_element(samples.begin(), samples.end()), 3000);
        EXPECT_GE(*std::min_element(samples.begin(), samples.end()), -3000);
        if (segment != 0) {
            EXPECT_NE(samples, state->submitted_samples[segment - 1]);
        }
        audio.updateCreationAudio(client::CreationAudioStage::Water, time + 1.84);
        EXPECT_EQ(state->submitted_samples.size(), segment + 1);
    }
    // A phase change immediately takes its own first recording segment.
    audio.updateCreationAudio(client::CreationAudioStage::Ice, 7.39);
    ASSERT_EQ(state->submitted_samples.size(), 5U);
    EXPECT_NE(state->submitted_samples.back(), state->submitted_samples.front());
}

TEST(ClientAudioTest, PackagedMusicLoopsWelcomeAndGameplayWithoutScheduledGap)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    std::vector<std::string> requested;
    ASSERT_TRUE(audio.loadMusic([&](std::string_view name) {
        requested.emplace_back(name);
        return musicWave(name == "welcome.wav" ? 1500 : 700);
    }));
    EXPECT_EQ(requested, (std::vector<std::string>{"welcome.wav", "gameplay.wav"}));
    ASSERT_TRUE(audio.start());
    EXPECT_FALSE(audio.loadMusic([](std::string_view) { return musicWave(1000); }));
    audio.updateCreationAudio(client::CreationAudioStage::Ready, 0.0);
    audio.updateCreationAudio(client::CreationAudioStage::Ready, 1.84);
    EXPECT_EQ(state->play_count, 1U);
    audio.updateCreationAudio(client::CreationAudioStage::Ready, 1.85);
    ASSERT_EQ(state->play_count, 2U);
    audio.updateCreationAudio(client::CreationAudioStage::Gameplay, 2.0);
    ASSERT_EQ(state->play_count, 3U);
    EXPECT_NE(state->submitted_samples[0], state->submitted_samples[2]);
    audio.updateCreationAudio(client::CreationAudioStage::Gameplay, 3.85);
    EXPECT_EQ(state->play_count, 4U);
}

TEST(ClientAudioTest, InvalidMusicHeaderDoesNotDisableMaterialSounds)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    EXPECT_FALSE(audio.loadMusic([](std::string_view) { return std::vector<uint8_t>{'R', 'I', 'F', 'F'}; }));
    ASSERT_TRUE(audio.start());
    EXPECT_TRUE(audio.playBlockSound(client::BlockSoundKind::Break, shared::Block::Stone));
}

TEST(ClientAudioTest, UnsupportedMusicRateIsRejected)
{
    client::ClientAudio audio{ std::make_unique<FakeOutput>(std::make_shared<FakeOutputState>()) };
    EXPECT_FALSE(audio.loadMusic([](std::string_view) {
        auto bytes = musicWave(1000);
        bytes[24] = 0; // The rate field is no longer 24 kHz, with otherwise valid RIFF.
        return bytes;
    }));
}

TEST(ClientAudioTest, TruncatedMusicChunkIsRejected)
{
    client::ClientAudio audio{ std::make_unique<FakeOutput>(std::make_shared<FakeOutputState>()) };
    EXPECT_FALSE(audio.loadMusic([](std::string_view) {
        auto bytes = musicWave(1000);
        bytes[40] = 0xff; // data payload exceeds the enclosing RIFF bounds.
        bytes[41] = 0xff;
        return bytes;
    }));
}

TEST(ClientAudioTest, MapTransitionsAndLandingHaveSeparateFadedCues)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    ASSERT_TRUE(audio.playMapSound(true));
    ASSERT_TRUE(audio.playMapSound(false));
    ASSERT_TRUE(audio.playBlockSound(client::BlockSoundKind::Landing, shared::Block::Dirt));
    ASSERT_EQ(state->submitted_samples.size(), 3U);
    EXPECT_NE(state->submitted_samples[0], state->submitted_samples[1]);
    EXPECT_NE(state->submitted_samples[1], state->submitted_samples[2]);
    for (auto const& samples : state->submitted_samples) {
        EXPECT_EQ(samples.front(), 0);
        EXPECT_EQ(samples.back(), 0);
    }
}

TEST(ClientAudioTest, ActionRolesHaveAudibleBoundedLevelsAndDistinctPlacementGesture)
{
    std::array<int, 5> const expected_peaks{900, 2600, 2200, 1800, 2400};
    std::array<std::vector<int16_t>, 5> clips;
    for (size_t kind = 0; kind < clips.size(); ++kind) {
        auto state = std::make_shared<FakeOutputState>();
        client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
        ASSERT_TRUE(audio.start());
        ASSERT_TRUE(audio.playBlockSound(static_cast<client::BlockSoundKind>(kind), shared::Block::Stone));
        clips[kind] = state->submitted_samples.back();
        int peak = 0;
        double energy = 0.0;
        for (int16_t sample : clips[kind]) {
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
            energy += static_cast<double>(sample) * sample;
        }
        EXPECT_GE(peak, expected_peaks[kind] - 1);
        EXPECT_LE(peak, expected_peaks[kind]);
        EXPECT_GT(std::sqrt(energy / clips[kind].size()), 100.0);
        EXPECT_EQ(clips[kind].front(), 0);
        EXPECT_EQ(clips[kind].back(), 0);
    }
    EXPECT_NE(clips[1], clips[2]);
    EXPECT_NE(clips[2], clips[3]);
    EXPECT_NE(clips[3], clips[4]);
    // All admitted voices at their maximum remain below signed-16 saturation.
    EXPECT_LT(8 * 3000, std::numeric_limits<int16_t>::max());
}

TEST(ClientAudioTest, MapCuesRemainAudibleAndOutputRejectionDoesNotPlayPlacement)
{
    auto state = std::make_shared<FakeOutputState>();
    client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
    ASSERT_TRUE(audio.start());
    ASSERT_TRUE(audio.playMapSound(true));
    int peak = 0;
    for (int16_t sample : state->submitted_samples.back()) {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    EXPECT_GE(peak, 2299);
    EXPECT_LE(peak, 2300);
    state->play_result = false;
    EXPECT_FALSE(audio.playBlockSound(client::BlockSoundKind::Place, shared::Block::Dirt));
}

TEST(ClientAudioTest, ExportsReviewClipsAndChecksWorstCaseVoiceMix)
{
    char const* directory = std::getenv("MC_AUDIO_REVIEW_DIRECTORY");
    if (directory == nullptr) {
        GTEST_SKIP() << "Set MC_AUDIO_REVIEW_DIRECTORY to export actual submitted PCM";
    }
    std::filesystem::create_directories(directory);
    auto write = [&](std::string const& name, std::span<int16_t const> samples) {
        std::ofstream file{std::filesystem::path{directory} / (name + ".wav"), std::ios::binary};
        auto u16 = [&](uint16_t value) {
            file.put(static_cast<char>(value));
            file.put(static_cast<char>(value >> 8));
        };
        auto u32 = [&](uint32_t value) { u16(static_cast<uint16_t>(value)); u16(value >> 16); };
        file.write("RIFF", 4); u32(static_cast<uint32_t>(36 + samples.size() * 2));
        file.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(48'000); u32(192'000); u16(4); u16(16);
        file.write("data", 4); u32(static_cast<uint32_t>(samples.size() * 2));
        for (auto sample : samples) {
            u16(static_cast<uint16_t>(sample));
        }
        EXPECT_TRUE(file.good());
    };
    constexpr std::array<std::string_view, 5> NAMES{"chip", "break", "place", "step", "land"};
    for (size_t kind = 0; kind < NAMES.size(); ++kind) {
        for (auto material : {shared::Block::Stone, shared::Block::Dirt}) {
            auto state = std::make_shared<FakeOutputState>();
            client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
            ASSERT_TRUE(audio.start());
            ASSERT_TRUE(audio.playBlockSound(static_cast<client::BlockSoundKind>(kind), material));
            write(std::string{NAMES[kind]} + (material == shared::Block::Stone ? "-stone" : "-dirt"),
                state->submitted_samples.back());
        }
    }
    for (uint8_t stage = 1; stage <= 5; ++stage) {
        auto state = std::make_shared<FakeOutputState>();
        client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
        ASSERT_TRUE(audio.start());
        audio.updateCreationAudio(static_cast<client::CreationAudioStage>(stage), 0.0);
        auto const& samples = state->submitted_samples.back();
        write("creation-" + std::to_string(stage), samples);
        std::vector<int16_t> mixed(samples.size());
        int mix_peak = 0;
        for (size_t sample = 0; sample < samples.size(); ++sample) {
            int const sum = 8 * static_cast<int>(samples[sample]);
            mix_peak = std::max(mix_peak, std::abs(sum));
            mixed[sample] = static_cast<int16_t>(sum);
        }
        EXPECT_LT(mix_peak, 32'768);
        write("eight-voices-" + std::to_string(stage), mixed);
    }
    for (bool open : {true, false}) {
        auto state = std::make_shared<FakeOutputState>();
        client::ClientAudio audio{ std::make_unique<FakeOutput>(state) };
        ASSERT_TRUE(audio.start());
        ASSERT_TRUE(audio.playMapSound(open));
        write(open ? "map-open" : "map-close", state->submitted_samples.back());
    }
}
