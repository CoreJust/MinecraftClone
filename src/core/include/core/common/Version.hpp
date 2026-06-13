#pragma once

#include <cstdint>
#include <tuple>

namespace core {

struct Version {
    uint32_t epoch = 0;
    uint32_t major = 1;
    uint32_t minor = 0;
    uint32_t patch = 0; // Or snapshot

    [[nodiscard]]
    constexpr auto operator<=>(Version const& lhs) const noexcept {
        return std::tie(epoch, major, minor, patch)
            <=> std::tie(lhs.epoch, lhs.major, lhs.minor, lhs.patch);
    }

    [[nodiscard]]
    static consteval Version max() noexcept {
        return Version {
            static_cast<uint32_t>(-1),
            static_cast<uint32_t>(-1),
            static_cast<uint32_t>(-1),
            static_cast<uint32_t>(-1),
        };
    }
};

} // namespace core
