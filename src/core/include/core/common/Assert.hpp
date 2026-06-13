#pragma once

#include <core/IO/Log.hpp>

namespace core::detail {

inline void assertFailed(
    char const* const file,
    int const line,
    char const* const condition_str
) {
    Log::getLogger()->critical(
        "Assertion failed at {}:{}\n{} evaluated to false\nStacktrace:\n{}",
        file, line, condition_str, std::stacktrace::current());
    std::exit(1);
}

template <typename... Args>
void assertFailed(
    char const* const file,
    int const line,
    char const* const condition_str,
    fmt::format_string<Args...> fmt,
    Args&&... args
) {
    std::string user_msg = fmt::format(fmt, std::forward<Args>(args)...);
    Log::getLogger()->critical(
        "Assertion failed at {}:{}: {}\n{} evaluated to false\nStacktrace:\n{}",
        file, line, user_msg, condition_str, std::stacktrace::current());
    std::exit(1);
}

} // namespace core::detail

#define ASSERT(condition, ...)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            ::core::detail::assertFailed(__FILE__, __LINE__,                  \
                #condition                                                    \
                __VA_OPT__(, __VA_ARGS__));                                   \
        }                                                                     \
    } while (0)

#ifdef _MC_ENABLE_HIGH_ASSERT
#define HIGH_ASSERT(condition, ...) ASSERT(condition, __VA_ARGS__)
#else
#define HIGH_ASSERT(...)
#endif
