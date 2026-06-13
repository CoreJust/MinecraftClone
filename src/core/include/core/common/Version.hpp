#pragma once

#include <cstdint>

namespace core {

struct Version {
    uint32_t epoch = 0;
    uint32_t major = 1;
    uint32_t minor = 0;
    uint32_t patch = 0; // Or snapshot

    [[nodiscard]]
    bool operator<(Version const& lhs) const noexcept;
    [[nodiscard]]
    constexpr bool operator==(Version const& lhs) const noexcept = default;

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
