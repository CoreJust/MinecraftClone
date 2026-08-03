#pragma once

#include <core/IO/Log.hpp>

#include <cstdio>

namespace core::detail {

[[noreturn]]
inline void assertFailed(
    char const* const file,
    int const line,
    char const* const condition_str
) {
    if (spdlog::logger* logger = Log::getLogger()) {
        logger->critical(
            "Assertion failed at {}:{}\n{} evaluated to false\nStacktrace:\n{}",
            file, line, condition_str, currentStacktrace()
        );
    } else {
        std::fprintf(stderr, "Assertion failed at %s:%d\n%s evaluated to false\n", file, line, condition_str);
    }
    std::exit(1);
}

template <typename... Args> [[noreturn]]
void assertFailed(
    char const* const file,
    int const line,
    char const* const condition_str,
    fmt::format_string<Args...> fmt,
    Args&&... args
) {
    std::string user_msg = fmt::format(fmt, std::forward<Args>(args)...);
    if (spdlog::logger* logger = Log::getLogger()) {
        logger->critical(
            "Assertion failed at {}:{}: {}\n{} evaluated to false\nStacktrace:\n{}",
            file, line, user_msg, condition_str, currentStacktrace()
        );
    } else {
        std::fprintf(stderr, "Assertion failed at %s:%d: %s\n%s evaluated to false\n", file, line, user_msg.c_str(), condition_str);
    }
    std::exit(1);
}

} // namespace core::detail

#define ASSERT(condition, ...)                               \
    do {                                                     \
        if (!(condition)) {                                  \
            ::core::detail::assertFailed(__FILE__, __LINE__, \
                #condition                                   \
                __VA_OPT__(, __VA_ARGS__));                  \
        }                                                    \
    } while (0)

#define UNREACHABLE(...)                                 \
    do {                                                 \
        ::core::detail::assertFailed(__FILE__, __LINE__, \
            "Unreachable" __VA_OPT__(, __VA_ARGS__));    \
    } while (0)

#ifdef _MC_ENABLE_HIGH_ASSERT
#define HIGH_ASSERT(condition, ...) ASSERT(condition, __VA_ARGS__)
#else
#define HIGH_ASSERT(...)
#endif
