#pragma once

#include <core/common/Version.hpp>

#include <fmt/core.h>

namespace fmt {

template<>
struct formatter<core::Version> : formatter<std::string_view> {
    context::iterator format(core::Version const& version, format_context& ctx) const {
        return format_to(ctx.out(), "{}.{}.{}:{}", version.epoch, version.major, version.minor, version.patch);
    }
};

} // namespace fmt
