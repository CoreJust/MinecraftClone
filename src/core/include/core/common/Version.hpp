#pragma once

#include <fmt/core.h>

#include <cstdint>
#include <tuple>

namespace core {

struct Version {
    uint32_t epoch = 0;
    uint32_t major = 1;
    uint32_t minor = 0;
    uint32_t patch = 0; // Or snapshot

    [[nodiscard]]
    constexpr auto operator<=>(Version const& rhs) const noexcept {
        return std::tie(epoch, major, minor, patch)
            <=> std::tie(rhs.epoch, rhs.major, rhs.minor, rhs.patch);
    }

    [[nodiscard]]
    static consteval Version MAX() noexcept {
        return Version {
            static_cast<uint32_t>(-1),
            static_cast<uint32_t>(-1),
            static_cast<uint32_t>(-1),
            static_cast<uint32_t>(-1),
        };
    }
};

} // namespace core

namespace fmt {

template<>
struct formatter<core::Version> : formatter<std::string_view> {
    context::iterator format(core::Version const& version, format_context& ctx) const {
        return format_to(ctx.out(), "{}.{}.{}:{}", version.epoch, version.major, version.minor, version.patch);
    }
};

} // namespace fmt
