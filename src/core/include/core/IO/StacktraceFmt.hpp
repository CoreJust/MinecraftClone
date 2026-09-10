#pragma once

#include <fmt/core.h>

#if __has_include(<stacktrace>)
#   include <stacktrace>
#   define CORE_HAS_STACKTRACE 1
#else
#   include <string_view>
#   define CORE_HAS_STACKTRACE 0
#endif

namespace core {

#if CORE_HAS_STACKTRACE
[[nodiscard]]
inline std::stacktrace currentStacktrace() {
    return std::stacktrace::current();
}
#else
[[nodiscard]]
inline std::string_view currentStacktrace() {
    return "<stacktrace unavailable>";
}
#endif

} // namespace core

#if CORE_HAS_STACKTRACE

namespace fmt {

template <>
struct formatter<std::stacktrace> : formatter<std::string_view> {
    context::iterator format(std::stacktrace const& st, format_context& ctx) const;
};

} // namespace fmt

#endif
