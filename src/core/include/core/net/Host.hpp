#pragma once

#include "Address.hpp"
#include "Event.hpp"
#include "SendMode.hpp"
#include <core/ControlFlow.hpp>

#include <enet/enet.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace core {

class Host final {
public:
    /*
     * Address is the address that peers can use to connect to this host.
     * If nullopt, no peers can connect, which makes this host a client.
     */
    Host(
        std::optional<Address> const address,
        size_t const max_connections,
        size_t const max_channels);
    ~Host();

    std::optional<NetEvent> poll(std::chrono::milliseconds const timeout = std::chrono::milliseconds::zero());
    void flush() noexcept;

    /*
     * Repeats poll for up to total_timeout in total.
     * For each polled event, the callback is called.
     * Returns the number of events polled.
     */
    size_t repeatedlyPoll(auto&& cb, std::chrono::milliseconds total_timeout = std::chrono::milliseconds::zero()) {
        namespace chr = std::chrono;
        using CallbackReturnType = decltype(cb(std::declval<NetEvent>()));

        chr::time_point end = chr::steady_clock::now() + total_timeout;
        size_t polled_count = 0;
        while (auto event = poll(total_timeout)) {
            if constexpr (std::is_same_v<CallbackReturnType, ControlFlow>) {
                if (cb(std::move(*event)) == ControlFlow::Break) {
                    break;
                }
            } else {
                static_assert(
                    std::is_same_v<CallbackReturnType, void>,
                    "repeatedlyPoll only accepts callbacks that return either void or ControlFlow");
                cb(std::move(*event));
            }
            
            ++polled_count;
            total_timeout = chr::duration_cast<chr::milliseconds>(end - chr::steady_clock::now());
            if (total_timeout.count() < 0) {
                break;
            }
        }
        return polled_count;
    }

    /* 
     * Sends a packet containing data to the peer over channel with given ID and with the given mode.
     * If peer is nullopt, the packet is sent to all the peers.
     * Returns true if the packet was successfully queued for sending and false otherwise.
     * Note that the packet is not sent immediately.
     * To trigger the actual sending, you must call either flush or poll.
     */
    bool send(
        std::optional<Peer> const peer,
        std::span<uint8_t const> const data,
        uint8_t const channel_id,
        SendMode const mode);

    // Must not be called for a server.
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
