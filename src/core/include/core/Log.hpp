#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stacktrace>

#include <fmt/core.h>
#include <spdlog/spdlog.h>

namespace fmt {

template <>
struct formatter<std::stacktrace> : formatter<std::string_view> {
    context::iterator format(std::stacktrace const& st, format_context& ctx) const;
};
    
} // namespace fmt

namespace core {

class Log final {
public:
    Log() = delete;

    static void ensureInit(
        std::optional<std::filesystem::path> const logs_dir = std::nullopt,
        spdlog::level::level_enum const initial_level = spdlog::level::info);
    static void destroy();

    static void setLogLevel(spdlog::level::level_enum const level) {
        if (s_logger) {
            s_logger->set_level(level);
        }
    }
    
    static spdlog::level::level_enum getLogLevel() noexcept {
        return s_logger ? s_logger->level() : spdlog::level::off;
    }

    static spdlog::logger* getLogger() noexcept { return s_logger.get(); }
private:
    static std::shared_ptr<spdlog::logger> s_logger;
};
    
} // namespace core

#define LOG(level, ...)                                                                         \
    if (::core::Log::getLogLevel() <= level) {                                                  \
        ::core::Log::getLogger()                                                                \
            ->log(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, level, __VA_ARGS__); \
    }

#define TRACE(...)    LOG(::spdlog::level::trace, __VA_ARGS__)
#define DEBUG(...)    LOG(::spdlog::level::debug, __VA_ARGS__)
#define INFO(...)     LOG(::spdlog::level::info, __VA_ARGS__)
#define WARN(...)     LOG(::spdlog::level::warn, __VA_ARGS__)
#define ERROR(...)    LOG(::spdlog::level::err, __VA_ARGS__)
#define CRITICAL(...) LOG(::spdlog::level::critical, __VA_ARGS__)
