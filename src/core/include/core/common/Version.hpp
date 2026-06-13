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
};

} // namespace core
