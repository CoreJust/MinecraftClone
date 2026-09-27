#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace client {

class ClientAudioOutput {
public:
    virtual ~ClientAudioOutput() = default;

    [[nodiscard]] virtual bool start() noexcept = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool play(std::span<int16_t const> samples) noexcept = 0;
};

class ClientAudio final {
public:
    explicit ClientAudio(std::unique_ptr<ClientAudioOutput> output = {});
    ~ClientAudio();

    ClientAudio(ClientAudio const&) = delete;
    ClientAudio& operator=(ClientAudio const&) = delete;
    ClientAudio(ClientAudio&&) = delete;
    ClientAudio& operator=(ClientAudio&&) = delete;

    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] bool playJoinCue() noexcept;
    void stop() noexcept;

private:
    std::unique_ptr<ClientAudioOutput> m_output;
    bool m_started = false;
};

} // namespace client
