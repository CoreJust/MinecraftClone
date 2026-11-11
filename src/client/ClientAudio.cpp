#include <client/ClientAudio.hpp>

namespace client {
namespace {

// CC0 edited field recordings; provenance and reproducible PCM are in assets/audio.
constexpr int16_t BOILING_BED[] = {
#include "../../assets/audio/boiling-bed.inc"
};
constexpr int16_t DIRT_IMPACT[] = {
#include "../../assets/audio/dirt-impact.inc"
};
constexpr int16_t ICE_BED[] = {
#include "../../assets/audio/ice-bed.inc"
};
constexpr int16_t RAIN_BED[] = {
#include "../../assets/audio/rain-bed.inc"
};
constexpr int16_t STONE_IMPACT[] = {
#include "../../assets/audio/stone-impact.inc"
};
constexpr int16_t WIND_BED[] = {
#include "../../assets/audio/wind-bed.inc"
};

} // namespace
} // namespace client

#include <core/audio/Audio.hpp>
#include <core/audio/AudioOutput.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace client {

namespace {

constexpr uint32_t SAMPLE_RATE = 48'000U;
constexpr uint32_t CHANNEL_COUNT = 2U;
constexpr double PI = 3.141'592'653'589'793'238'46;
using Clip = std::vector<int16_t>;

// Original deterministic synthesis: no recorded or third-party sample sources.
// Every clip has zero endpoints and a raised-cosine fade. Eight admitted voices,
// each <= 3000 peak, leave > 2 dB headroom below signed-16 saturation.
Clip makeClip(size_t frames, uint32_t seed, int texture, double pitch, double strength)
{
    Clip samples(frames * CHANNEL_COUNT);
    double low = 0.0;
    double slow = 0.0;
    double previous = 0.0;
    for (size_t frame = 0; frame < frames; ++frame) {
        seed = seed * 1'664'525U + 1'013'904'223U;
        double const noise = static_cast<double>(seed >> 8U) / 8'388'607.5 - 1.0;
        constexpr std::array<double, 7> MATERIAL_FILTERS{0.12, 0.30, 0.10, 0.035, 0.22, 0.055, 0.40};
        double const filter = texture >= 5 ? MATERIAL_FILTERS[static_cast<size_t>(texture - 5)] : 0.035;
        low += filter * (noise - low);
        slow += 0.002 * (noise - slow);
        double const time = static_cast<double>(frame) / SAMPLE_RATE;
        double const fraction = static_cast<double>(frame) / static_cast<double>(frames - 1);
        double const edge = std::min({1.0, fraction * 8.0, (1.0 - fraction) * 8.0});
        double const fade = 0.5 - 0.5 * std::cos(PI * edge);
        double sound = 0.0;
        if (texture == -1) { // Connection acknowledgement: a short, unpitched soft tap.
            sound = 2.2 * low * std::exp(-7.0 * fraction);
        } else if (texture == 0) { // Molten: low air, slow pressure, scattered crackle.
            sound = 2.3 * slow + 0.18 * low + 0.035 * std::sin(2.0 * PI * 43.0 * time);
            sound += std::abs(noise) > 0.998 ? noise * 0.20 : 0.0;
        } else if (texture == 1) { // Cooling rock / ice: diffuse bed and damped creaks.
            double const pulse = std::pow(0.5 + 0.5 * std::sin(2.0 * PI * 3.1 * time), 18.0);
            sound = 0.7 * low + 0.16 * pulse * std::sin(2.0 * PI * pitch * time + 2.0 * slow);
        } else if (texture == 2) { // Water: soft wide rain with a low swell.
            sound = 0.22 * noise + 0.8 * low + 0.6 * slow;
        } else if (texture == 3) { // Living coast: wind, ocean wash and a distant bird phrase.
            double const wash = 0.55 + 0.45 * std::sin(2.0 * PI * 0.7 * time);
            double const bird = std::pow(std::max(0.0, std::sin(2.0 * PI * 1.1 * time)), 12.0);
            sound = wash * (1.8 * slow + 0.45 * low)
                + 0.018 * bird * std::sin(2.0 * PI * (1600.0 * time + 90.0 * time * time));
        } else if (texture == 4) { // Ready: breathy, decaying consonant chord.
            sound = std::exp(-4.0 * fraction) * (0.13 * std::sin(2.0 * PI * 220.0 * time)
                + 0.09 * std::sin(2.0 * PI * 330.0 * time)
                + 0.06 * std::sin(2.0 * PI * 440.0 * time) + 0.1 * low);
        } else { // Material impact: filtered grains and damped, inharmonic body.
            double const decay = std::exp(-fraction * 7.0);
            double const body = std::sin(2.0 * PI * pitch * time)
                + 0.35 * std::sin(2.0 * PI * pitch * 1.47 * time);
            constexpr std::array<double, 7> BODY_GAINS{0.19, 0.035, 0.08, 0.22, 0.015, 0.24, 0.025};
            double const grains = texture == 10 ? low * 0.8 : low * 1.3;
            sound = decay * (grains + BODY_GAINS[static_cast<size_t>(texture - 5)] * body);
        }
        double const sample = std::clamp(sound * fade * strength, -3000.0, 3000.0);
        // A tiny filtered side component gives space without a hard stereo echo.
        samples[frame * 2] = static_cast<int16_t>(sample);
        samples[frame * 2 + 1] = static_cast<int16_t>(0.96 * sample + 0.04 * previous);
        previous = sample;
    }
    samples.back() = 0;
    return samples;
}

Clip recordedImpact(std::span<int16_t const> recording, double gain)
{
    Clip samples(recording.size() * 4);
    for (size_t frame = 0; frame < recording.size() * 2; ++frame) {
        auto const first = recording[frame / 2];
        auto const second = recording[std::min(frame / 2 + 1, recording.size() - 1)];
        double const value = frame % 2 == 0 ? first : (static_cast<double>(first) + second) * 0.5;
        auto const sample = static_cast<int16_t>(value * gain);
        samples[frame * 2] = sample;
        samples[frame * 2 + 1] = sample;
    }
    return samples;
}

Clip recordedBedSegment(std::span<int16_t const> recording, size_t segment, double gain)
{
    constexpr size_t SOURCE_FRAMES = 48'000;
    constexpr size_t SOURCE_ADVANCE = 44'400;
    constexpr size_t FADE_FRAMES = 3'600;
    auto samples = recordedImpact(recording.subspan(segment * SOURCE_ADVANCE, SOURCE_FRAMES), gain);
    for (size_t frame = 0; frame < SOURCE_FRAMES * 2; ++frame) {
        double const fade = std::min({1.0, static_cast<double>(frame) / (FADE_FRAMES * 2),
            static_cast<double>(SOURCE_FRAMES * 2 - 1 - frame) / (FADE_FRAMES * 2)});
        samples[frame * 2] = static_cast<int16_t>(samples[frame * 2] * fade);
        samples[frame * 2 + 1] = samples[frame * 2];
    }
    return samples;
}

std::vector<Clip> decodeMusic(std::span<uint8_t const> bytes)
{
    constexpr size_t MAX_MUSIC_BYTES = 8 * 1024 * 1024;
    auto matches = [&](size_t offset, std::string_view tag) {
        return offset <= bytes.size() && tag.size() <= bytes.size() - offset
            && std::equal(tag.begin(), tag.end(), bytes.begin() + offset);
    };
    auto u16 = [&](size_t offset) {
        return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    };
    auto u32 = [&](size_t offset) {
        return static_cast<uint32_t>(u16(offset)) | (static_cast<uint32_t>(u16(offset + 2)) << 16);
    };
    if (bytes.size() < 12 || bytes.size() > MAX_MUSIC_BYTES || !matches(0, "RIFF")
        || !matches(8, "WAVE") || u32(4) != bytes.size() - 8) {
        throw std::runtime_error("Music asset must be a bounded RIFF WAVE file");
    }
    bool format_valid = false;
    std::span<uint8_t const> data;
    for (size_t offset = 12; offset < bytes.size();) {
        if (bytes.size() - offset < 8) {
            throw std::runtime_error("Truncated music chunk");
        }
        size_t const size = u32(offset + 4);
        size_t const begin = offset + 8;
        if (size > bytes.size() - begin) {
            throw std::runtime_error("Truncated music payload");
        }
        if (matches(offset, "fmt ")) {
            format_valid = size >= 16 && u16(begin) == 1 && u16(begin + 2) == 1
                && u32(begin + 4) == 24'000 && u32(begin + 8) == 48'000
                && u16(begin + 12) == 2 && u16(begin + 14) == 16;
            if (!format_valid) {
                throw std::runtime_error("Music must be PCM16 mono at 24 kHz");
            }
        } else if (matches(offset, "data")) {
            if (!data.empty()) {
                throw std::runtime_error("Duplicate music data");
            }
            data = bytes.subspan(begin, size);
        }
        size_t const padded = size + (size % 2);
        if (padded > bytes.size() - begin) {
            throw std::runtime_error("Missing music chunk padding");
        }
        offset = begin + padded;
    }
    if (!format_valid || data.size() < 2 || data.size() % 2 != 0) {
        throw std::runtime_error("Music has no valid PCM payload");
    }
    std::vector<int16_t> mono(data.size() / 2);
    for (size_t sample = 0; sample < mono.size(); ++sample) {
        auto const encoded = static_cast<uint16_t>(data[sample * 2]
            | (static_cast<uint16_t>(data[sample * 2 + 1]) << 8));
        // Keep music under the same per-voice headroom budget as material sounds.
        mono[sample] = std::clamp(std::bit_cast<int16_t>(encoded), int16_t{-2600}, int16_t{2600});
    }
    // Blend the ending over the opening, then cycle the resulting source without
    // zero padding. This removes the edited excerpts' long silent endpoint fades.
    size_t const overlap = std::min<size_t>(48'000, mono.size() / 4);
    if (overlap != 0) {
        for (size_t frame = 0; frame < overlap; ++frame) {
            double const blend = static_cast<double>(frame) / overlap;
            mono[frame] = static_cast<int16_t>(mono[mono.size() - overlap + frame] * (1.0 - blend)
                + mono[frame] * blend);
        }
        mono.resize(mono.size() - overlap);
    }
    size_t const segment_count = std::max<size_t>(1, (mono.size() + 44'399) / 44'400);
    size_t const cycle_frames = segment_count * 44'400;
    std::vector<int16_t> cycle(cycle_frames);
    for (size_t frame = 0; frame < cycle_frames; ++frame) {
        cycle[frame] = mono[frame * mono.size() / cycle_frames];
    }
    std::vector<Clip> segments;
    for (size_t segment = 0; segment < segment_count; ++segment) {
        std::array<int16_t, 48'000> window{};
        for (size_t frame = 0; frame < window.size(); ++frame) {
            window[frame] = cycle[(segment * 44'400 + frame) % cycle_frames];
        }
        segments.push_back(recordedBedSegment(window, 0, 1.0));
    }
    return segments;
}

void softenImpact(Clip& samples, double gain, double attack_seconds)
{
    double low = 0.0;
    size_t const frames = samples.size() / CHANNEL_COUNT;
    for (size_t frame = 0; frame < frames; ++frame) {
        low += 0.14 * (samples[frame * 2] - low);
        double const attack = std::min(1.0, static_cast<double>(frame) / (SAMPLE_RATE * attack_seconds));
        double const release = std::min(1.0, static_cast<double>(frames - 1 - frame) / 480.0);
        auto const value = static_cast<int16_t>(low * gain * attack * release);
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
}

void normalizeCue(Clip& samples, double peak)
{
    int maximum = 0;
    for (auto sample : samples) {
        maximum = std::max(maximum, std::abs(static_cast<int>(sample)));
    }
    if (maximum == 0) {
        return;
    }
    for (auto& sample : samples) {
        sample = static_cast<int16_t>(sample * (peak / maximum));
    }
}

Clip materialCue(std::span<int16_t const> recording, BlockSoundKind kind, size_t material)
{
    // Separate gestures: a long distributed crumble, a compact placement tap,
    // a low rolling foot contact, and a broader landing contact.
    constexpr std::array<size_t, 5> FRAMES{5760, 28'800, 8640, 12'960, 16'800};
    constexpr std::array<double, 5> PEAKS{900, 2600, 2200, 1800, 2400};
    auto const action = static_cast<size_t>(kind);
    Clip samples(FRAMES[action] * CHANNEL_COUNT);
    double low = 0.0;
    for (size_t frame = 0; frame < FRAMES[action]; ++frame) {
        double const fraction = static_cast<double>(frame) / (FRAMES[action] - 1);
        // Skip the initial hard strike for crumbling and walking; preserve body.
        double const start = kind == BlockSoundKind::Break || kind == BlockSoundKind::Footstep ? 0.16 : 0.0;
        size_t const source = std::min(recording.size() - 1, static_cast<size_t>(
            (start + (1.0 - start) * fraction) * (recording.size() - 1)));
        double const filter = kind == BlockSoundKind::Footstep ? 0.08 : 0.18;
        low += filter * (recording[source] - low);
        double const attack = kind == BlockSoundKind::Break ? 0.14 : 0.09;
        double const envelope = std::sin(PI * std::min(1.0, fraction / attack) * 0.5)
            * std::pow(1.0 - fraction, kind == BlockSoundKind::Place ? 2.0 : 0.7);
        double const texture = kind == BlockSoundKind::Break
            ? 0.65 + 0.35 * std::sin(PI * fraction * (5.0 + material)) : 1.0;
        // A gentle soft knee keeps quiet chip grains present without restoring
        // the recording's isolated sharp strike. The action peak stays at 900.
        double const body = kind == BlockSoundKind::Chip ? low / (1.0 + std::abs(low) / 600.0) : low;
        auto const value = static_cast<int16_t>(body * envelope * texture);
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    normalizeCue(samples, PEAKS[action]);
    return samples;
}

struct SoundBank final {
    Clip connection;
    std::array<Clip, 2> map;
    std::array<Clip, 6> creation;
    std::array<std::array<Clip, 4>, 5> recorded_creation;
    std::array<std::array<Clip, 7>, 5> material;

    SoundBank()
    {
        map[0] = recordedImpact(DIRT_IMPACT, 0.16);
        map[1] = map[0];
        std::reverse(map[1].begin(), map[1].end());
        softenImpact(map[0], 1.0, 0.025);
        softenImpact(map[1], 1.0, 0.025);
        normalizeCue(map[0], 2300);
        normalizeCue(map[1], 2300);
        connection = makeClip(5760, 707, -1, 0, 3000);
        creation[0] = makeClip(57'600, 101, 0, 70, 4200);
        creation[1] = makeClip(57'600, 202, 1, 117, 3500);
        creation[2] = makeClip(57'600, 303, 2, 0, 2500);
        creation[3] = makeClip(57'600, 404, 1, 671, 2600);
        creation[4] = makeClip(57'600, 505, 3, 0, 4800);
        creation[5] = makeClip(33'600, 606, 4, 0, 3800);
        for (size_t segment = 0; segment < 4; ++segment) {
            recorded_creation[0][segment] = recordedBedSegment(BOILING_BED, segment, 1.0);
            recorded_creation[1][segment] = recordedBedSegment(BOILING_BED, segment, 0.45);
            recorded_creation[2][segment] = recordedBedSegment(RAIN_BED, segment, 0.75);
            recorded_creation[3][segment] = recordedBedSegment(ICE_BED, segment, 0.80);
            recorded_creation[4][segment] = recordedBedSegment(WIND_BED, segment, 0.75);
        }
        for (size_t stage = 0; stage < recorded_creation.size(); ++stage) {
            for (auto& clip : recorded_creation[stage]) {
                normalizeCue(clip, stage == 1 ? 2600 : 3000);
            }
        }
        for (size_t kind = 0; kind < material.size(); ++kind) {
            for (size_t block = 0; block < material[kind].size(); ++block) {
                bool const mineral = block == 0 || block == 3 || block == 5;
                std::span<int16_t const> recording = mineral
                    ? std::span<int16_t const>{STONE_IMPACT} : std::span<int16_t const>{DIRT_IMPACT};
                material[kind][block] = materialCue(recording, static_cast<BlockSoundKind>(kind), block);
            }
        }
    }
};

SoundBank const& soundBank()
{
    // Constructed once during ClientAudio construction, never in update/event paths.
    static SoundBank const bank;
    return bank;
}

double steadySeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class RuntimeClientAudioOutput final : public ClientAudioOutput {
public:
    [[nodiscard]] bool start() noexcept override { return m_output.start(); }
    void stop() noexcept override { m_output.stop(); }
    [[nodiscard]] bool play(std::span<int16_t const> samples) noexcept override
    {
        return m_output.play({
            .format = {
                .sample_format = core::audio::SampleFormat::Signed16,
                .channel_count = CHANNEL_COUNT,
                .sample_rate = SAMPLE_RATE,
            },
            .samples = samples,
        });
    }
private:
    core::audio::output::RuntimeAudioOutput m_output{
        core::audio::AudioMixerDescriptor{.max_voices = 8U}
    };
};

} // namespace

ClientAudio::ClientAudio(std::unique_ptr<ClientAudioOutput> output)
    : m_output{ output ? std::move(output) : std::make_unique<RuntimeClientAudioOutput>() }
{
    (void)soundBank();
}

ClientAudio::~ClientAudio() { stop(); }

bool ClientAudio::loadMusic(AudioAssetLoader const& loader) noexcept
{
    if (m_started || !loader) {
        return false;
    }
    try {
        std::array<std::vector<Clip>, 2> music;
        music[0] = decodeMusic(loader("welcome.wav"));
        music[1] = decodeMusic(loader("gameplay.wav"));
        m_music = std::move(music);
        return true;
    } catch (...) {
        return false;
    }
}

bool ClientAudio::start() noexcept
{
    if (!m_started) {
        m_started = m_output->start();
    }
    return m_started;
}

bool ClientAudio::submit(std::span<int16_t const> samples) noexcept
{
    if (!m_started) {
        return false;
    }
    double const now = steadySeconds();
    auto const free_voice = std::find_if(m_voice_end_seconds.begin(), m_voice_end_seconds.end(),
        [now](double end) { return end <= now; });
    if (free_voice == m_voice_end_seconds.end() || !m_output->play(samples)) {
        return false;
    }
    *free_voice = now + static_cast<double>(samples.size()) / (SAMPLE_RATE * CHANNEL_COUNT);
    return true;
}

bool ClientAudio::playJoinCue() noexcept
{
    return submit(soundBank().connection);
}

void ClientAudio::updateCreationAudio(CreationAudioStage stage, double now_seconds) noexcept
{
    if (!m_started || !std::isfinite(now_seconds)
        || static_cast<unsigned>(stage) > static_cast<unsigned>(CreationAudioStage::Gameplay)) {
        return;
    }
    bool const changed = stage != m_creation_stage;
    if (stage == CreationAudioStage::Silent) {
        // Existing clips release through their baked fade (at most 2 seconds).
        // Do not restart the device or abruptly cut a nonzero waveform.
        m_creation_stage = stage;
        m_next_creation_seconds = 0.0;
        return;
    }
    if (changed) {
        m_creation_stage = stage;
        m_next_creation_seconds = now_seconds;
        m_creation_segment = 0;
        m_music_segment = 0;
    }
    if (now_seconds < m_next_creation_seconds) {
        return;
    }
    if (stage == CreationAudioStage::Ready || stage == CreationAudioStage::Gameplay) {
        auto const music_index = stage == CreationAudioStage::Ready ? 0U : 1U;
        auto const& music = m_music[music_index];
        if (!music.empty()) {
            if (submit(music[m_music_segment])) {
                ++m_music_segment;
                if (m_music_segment == music.size()) {
                    m_music_segment = 0;
                    m_next_creation_seconds = now_seconds + 1.85;
                } else {
                    m_next_creation_seconds = now_seconds + 1.85;
                }
            } else {
                m_next_creation_seconds = now_seconds + 0.25;
            }
            return;
        }
        if (stage == CreationAudioStage::Gameplay) {
            return;
        }
    }
    auto const index = static_cast<size_t>(stage) - 1;
    auto const& clip = index < 5
        ? soundBank().recorded_creation[index][m_creation_segment] : soundBank().creation[index];
    if (submit(clip)) {
        m_creation_segment = (m_creation_segment + 1) % 4;
        m_next_creation_seconds = stage == CreationAudioStage::Ready
            ? std::numeric_limits<double>::infinity() : now_seconds + (index < 5 ? 1.85 : 0.90);
    } else {
        // Device pressure never creates catch-up bursts or a per-frame retry loop.
        m_next_creation_seconds = now_seconds + 0.25;
    }
}

bool ClientAudio::playBlockSound(BlockSoundKind kind, shared::Block material) noexcept
{
    auto const kind_index = static_cast<size_t>(kind);
    auto const material_index = static_cast<size_t>(material);
    double const now = steadySeconds();
    if (kind_index >= 5 || material_index == 0 || material_index > 7 || now < m_next_block_seconds[kind_index]) {
        return false;
    }
    if (!submit(soundBank().material[kind_index][material_index - 1])) {
        return false;
    }
    m_next_block_seconds[kind_index] = now + (kind == BlockSoundKind::Chip ? 0.085
        : kind == BlockSoundKind::Footstep ? 0.18 : 0.035);
    return true;
}

bool ClientAudio::playMapSound(bool opened) noexcept
{
    return submit(soundBank().map[opened ? 0 : 1]);
}

void ClientAudio::stop() noexcept
{
    if (m_started) {
        m_output->stop();
        m_started = false;
    }
    m_creation_stage = CreationAudioStage::Silent;
    m_next_creation_seconds = 0.0;
    m_voice_end_seconds.fill(0.0);
    m_next_block_seconds.fill(0.0);
}

} // namespace client
