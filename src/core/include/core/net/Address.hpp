#pragma once

#include <enet/enet.h>
#include <fmt/core.h>

#include <optional>
#include <string>

namespace core {

class Address final {
public:
    [[nodiscard]]
    constexpr static Address fromRaw(ENetAddress const address) noexcept {
        return Address{ address };
    }

    [[nodiscard]]
    static Address make(std::string const& ip, uint16_t const port) noexcept;
    [[nodiscard]]
    static Address anyhost(uint16_t const port) noexcept;
    [[nodiscard]]
    static Address localhost(uint16_t const port) noexcept;

    [[nodiscard]]
    std::string ip() const noexcept;
    [[nodiscard]]
    constexpr uint16_t port() const noexcept { return m_address.port; }
    [[nodiscard]]
    constexpr ENetAddress const& raw() const& noexcept { return m_address; }
private:
    constexpr Address(ENetAddress const address) noexcept : m_address(address) { }
private:
    ENetAddress m_address;
};

} // namespace core

namespace fmt {

template <>
struct formatter<core::Address> : formatter<std::string_view> {
    context::iterator format(core::Address const& a, format_context& ctx) const;
};

} // namespace fmt
