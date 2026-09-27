#include <client/ClientAudio.hpp>

#include <core/audio/AudioOutput.hpp>

#include <array>
#include <utility>

namespace client {

namespace {

static constexpr uint32_t JOIN_CUE_FRAME_COUNT = 480U;
static constexpr uint32_t JOIN_CUE_CHANNEL_COUNT = 2U;
static constexpr uint32_t JOIN_CUE_PERIOD_FRAMES = 80U;
static constexpr int32_t JOIN_CUE_AMPLITUDE = 1'800;

static constexpr auto JOIN_CUE_SAMPLES = [] {
    std::array<int16_t, JOIN_CUE_FRAME_COUNT * JOIN_CUE_CHANNEL_COUNT> samples{ };
    for (uint32_t frame = 0U; frame < JOIN_CUE_FRAME_COUNT; ++frame) {
        int32_t const envelope = (JOIN_CUE_FRAME_COUNT - frame) * JOIN_CUE_AMPLITUDE
            / JOIN_CUE_FRAME_COUNT;
        int16_t const value = static_cast<int16_t>(
            frame % JOIN_CUE_PERIOD_FRAMES < JOIN_CUE_PERIOD_FRAMES / 2U
                ? envelope
                : -envelope
        );
        samples[frame * JOIN_CUE_CHANNEL_COUNT] = value;
        samples[frame * JOIN_CUE_CHANNEL_COUNT + 1U] = value;
    }
    return samples;
}();

class RuntimeClientAudioOutput final : public ClientAudioOutput {
public:
    [[nodiscard]] bool start() noexcept override
    {
        return m_output.start();
    }

    void stop() noexcept override
    {
        m_output.stop();
    }

    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        return m_output.play({
            .format = {
                .sample_format = core::audio::SampleFormat::Signed16,
                .channel_count = JOIN_CUE_CHANNEL_COUNT,
                .sample_rate = 48'000U,
            },
            .samples = samples,
        });
    }

private:
    core::audio::output::RuntimeAudioOutput m_output;
};

} // namespace

ClientAudio::ClientAudio(std::unique_ptr<ClientAudioOutput> output)
    : m_output{ output ? std::move(output) : std::make_unique<RuntimeClientAudioOutput>() }
{ }

ClientAudio::~ClientAudio()
{
    stop();
}

bool ClientAudio::start() noexcept
{
    if (m_started) {
        return true;
    }
    m_started = m_output->start();
    return m_started;
}

bool ClientAudio::playJoinCue() noexcept
{
    if (!m_started) {
        return false;
    }
    return m_output->play(JOIN_CUE_SAMPLES);
}

void ClientAudio::stop() noexcept
{
    if (!m_started) {
        return;
    }
    m_output->stop();
    m_started = false;
}

} // namespace client
