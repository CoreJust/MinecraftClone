#pragma once

#include <stacktrace>

#include <fmt/core.h>

namespace fmt {

template <>
struct formatter<std::stacktrace> : formatter<std::string_view> {
    context::iterator format(std::stacktrace const& st, format_context& ctx) const;
};

} // namespace fmt
