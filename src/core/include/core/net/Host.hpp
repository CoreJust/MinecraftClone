#pragma once

#include "Event.hpp"
#include "SendMode.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

using ENetHost = struct _ENetHost;

namespace core {

class Host final {
public:
    Host(
        std::optional<std::string> const& ip,
        std::optional<uint16_t> const port,
        size_t const max_connections,
        size_t const max_channels);
    ~Host();

    std::optional<NetEvent> poll(std::chrono::milliseconds const timeout = std::chrono::milliseconds::zero());
    void flush() noexcept;

    bool send(
        std::optional<Peer> const peer,
        std::span<uint8_t const> const data,
        uint8_t const channel_id,
        SendMode const mode);

    std::optional<Peer> connect(
        std::string const& ip,
        uint16_t const port,
        size_t const channels_count);
    void disconnect(Peer const peer) noexcept;
    void reset(Peer const peer) noexcept;

    [[nodiscard]]
    uint16_t port() const noexcept;
    [[nodiscard]]
    std::string_view ip() const noexcept;

    [[nodiscard]]
    constexpr bool isValid() const noexcept { return m_host != nullptr; }
private:
    ENetHost* m_host{ nullptr };
};

} // namespace core
