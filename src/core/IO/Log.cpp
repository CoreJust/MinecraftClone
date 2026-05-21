#include <core/IO/Log.hpp>

#include <core/AtAppExit.hpp>

#include <chrono>

#include <spdlog/async.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace core {

std::shared_ptr<spdlog::logger> Log::s_logger;

void Log::ensureInit(
    std::optional<std::filesystem::path> const logs_dir,
    spdlog::level::level_enum const initial_level
) {
    spdlog::init_thread_pool(1024 * 8, 1);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(initial_level);

    std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file_sink;
    if (logs_dir) {
        std::filesystem::create_directories(*logs_dir);
        auto log_path = *logs_dir / "game.log";
        file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string());
        file_sink->set_level(spdlog::level::trace);
    }

    std::vector<spdlog::sink_ptr> sinks { console_sink };
    if (file_sink) {
        sinks.push_back(file_sink);
    }
    s_logger = std::make_shared<spdlog::logger>("game", sinks.begin(), sinks.end());
    spdlog::register_logger(s_logger);

    s_logger->set_pattern("%^[%H:%M:%S.%e %l at %s:%# t%t] %v%$");
    s_logger->flush_on(spdlog::level::err);

    SPDLOG_LOGGER_INFO(
        s_logger,
        "Log started at {:%Y-%m-%d %H:%M:%S}",
        std::chrono::system_clock::now());
    core::AtAppExit{ destroy };
}

void Log::destroy() {
    if (s_logger) {
        s_logger->flush();
        s_logger.reset();
        spdlog::drop_all();
        spdlog::shutdown();
    }
}

} // namespace core
