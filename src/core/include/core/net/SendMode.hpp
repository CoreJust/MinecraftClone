#pragma once

#include <concepts>
#include <cstdint>

namespace core {

class SendMode final {
public:
    enum Flag : uint32_t {
        Reliable           = ENET_PACKET_FLAG_RELIABLE,
        Unsequenced        = ENET_PACKET_FLAG_UNSEQUENCED,
        NoAllocate         = ENET_PACKET_FLAG_NO_ALLOCATE,
        UnreliableFragment = ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT,
    };
private:
    // A workaround to make MSVC work
    static constexpr uint32_t collectFlags(auto... flags) noexcept { return (0 | ... | flags); }
public:
    template<std::same_as<Flag>... Flags>
    constexpr explicit SendMode(Flags const... flags) noexcept
        : m_value(collectFlags(flags...))
    { }

    [[nodiscard]]
    constexpr SendMode withSet(Flag const flag) const noexcept {
        SendMode result = *this;
        result.m_value |= flag;
        return result;
    }

    [[nodiscard]]
    constexpr SendMode withClear(Flag const flag) const noexcept {
        SendMode result = *this;
        result.m_value &= ~flag;
        return result;
    }

    [[nodiscard]]
    constexpr bool isSet(Flag const flag) const noexcept {
        return m_value & flag;
    }

    [[nodiscard]]
    constexpr bool isValid() const noexcept {
        return !(isSet(Reliable) && isSet(Unsequenced));
    }

    [[nodiscard]]
    constexpr uint32_t raw() const noexcept { return m_value; }
private:
    uint32_t m_value = 0;
};

} // namespace core
