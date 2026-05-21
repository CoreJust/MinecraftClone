#pragma once

#include "Address.hpp"
#include "Event.hpp"
#include "SendMode.hpp"

#include <enet/enet.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace core {

class Host final {
public:
    Host(
        std::optional<Address> const address,
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

    std::optional<Peer> connect(Address const address, size_t const channels_count);
    void disconnect(Peer const peer) noexcept;
    void reset(Peer const peer) noexcept;

    [[nodiscard]]
    uint16_t port() const noexcept;
    [[nodiscard]]
    std::string ip() const noexcept;

    [[nodiscard]]
    constexpr bool isValid() const noexcept { return m_host != nullptr; }
private:
    ENetHost* m_host{ nullptr };
};

} // namespace core
