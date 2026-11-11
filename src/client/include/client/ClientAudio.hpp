#pragma once

#include <shared/world/Chunk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace client {

class ClientAudioOutput {
public:
    virtual ~ClientAudioOutput() = default;

    [[nodiscard]] virtual bool start() noexcept = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool play(std::span<int16_t const> samples) noexcept = 0;
};

enum class CreationAudioStage : uint8_t { Silent, Molten, Cooling, Water, Ice, Living, Ready, Gameplay };
enum class BlockSoundKind : uint8_t { Chip, Break, Place, Footstep, Landing };

using AudioAssetLoader = std::function<std::vector<uint8_t>(std::string_view)>;

class ClientAudio final {
public:
    explicit ClientAudio(std::unique_ptr<ClientAudioOutput> output = {});
    ~ClientAudio();

    ClientAudio(ClientAudio const&) = delete;
    ClientAudio& operator=(ClientAudio const&) = delete;
    ClientAudio(ClientAudio&&) = delete;
    ClientAudio& operator=(ClientAudio&&) = delete;

    // Load welcome.wav and gameplay.wav before start(); failed loads preserve SFX.
    [[nodiscard]] bool loadMusic(AudioAssetLoader const& loader) noexcept;
    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] bool playJoinCue() noexcept;
    [[nodiscard]] bool playMapSound(bool opened) noexcept;
    void stop() noexcept;
    // Serialized client-thread API. Time is monotonic presentation time in seconds.
    void updateCreationAudio(CreationAudioStage stage, double now_seconds) noexcept;
    [[nodiscard]] bool playBlockSound(BlockSoundKind kind, shared::Block material) noexcept;

private:
    [[nodiscard]] bool submit(std::span<int16_t const> samples) noexcept;

    std::unique_ptr<ClientAudioOutput> m_output;
    bool m_started = false;
    CreationAudioStage m_creation_stage = CreationAudioStage::Silent;
    double m_next_creation_seconds = 0.0;
    size_t m_creation_segment = 0;
    std::array<std::vector<std::vector<int16_t>>, 2> m_music;
    size_t m_music_segment = 0;
    std::array<double, 8> m_voice_end_seconds{};
    std::array<double, 5> m_next_block_seconds{};
};

} // namespace client
