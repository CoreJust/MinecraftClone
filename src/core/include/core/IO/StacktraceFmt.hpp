#pragma once

#include <fmt/core.h>

#include <stacktrace>

namespace fmt {

template <>
struct formatter<std::stacktrace> : formatter<std::string_view> {
    context::iterator format(std::stacktrace const& st, format_context& ctx) const;
};

} // namespace fmt
