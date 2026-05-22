#include <core/net/Address.hpp>

#include <core/Assert.hpp>

namespace core {

[[nodiscard]]
Address Address::make(std::string const& ip, uint16_t const port) noexcept {
    ENetAddress result {
        .port = port,
    };
    enet_address_set_host_ip(&result, ip.c_str());
    return Address{ result };
}

[[nodiscard]]
Address Address::anyhost(uint16_t const port) noexcept {
    return Address{
        ENetAddress {
            .host = ENET_HOST_ANY,
            .port = port,
        },
    };
}

[[nodiscard]]
Address Address::localhost(uint16_t const port) noexcept {
    return Address::make("127.0.0.1", port);
}

[[nodiscard]]
std::string Address::ip() const noexcept {
    char buffer[64];
    ASSERT(!enet_address_get_host_ip(&m_address, buffer, 64));
    return std::string{ buffer };
}

} // namespace core

namespace fmt {

context::iterator formatter<core::Address>::format(core::Address const& a, format_context& ctx) const {
    return format_to(ctx.out(), "{}:{}", a.ip(), a.port());
}

} // namespace fmt
