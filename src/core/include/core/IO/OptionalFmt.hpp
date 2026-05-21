#pragma once

#include <optional>

#include <fmt/core.h>

namespace fmt {

template <typename T>
struct formatter<std::optional<T>> : formatter<std::string_view> {
    context::iterator format(std::optional<T> const& opt, format_context& ctx) const {
        if (!opt.has_value()) {
            return format_to(ctx.out(), "nullopt");
        } else {
            return format_to(ctx.out(), "{}", *opt);
        }
    }
};

} // namespace fmt
