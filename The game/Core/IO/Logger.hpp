#pragma once
#include <string>
#include <string_view>
#include <format>

namespace core::io {
	enum class LogLevel : uint8_t {
		Trace,
		Debug,
		Info,
		Warn,
		Error,
		Fatal,
	};

	// Not thread safe, must be called from a single thread at a time
	void setLogLevel(LogLevel level);
	void log(LogLevel level, const std::string& msg);

	template<class... Args>
	void trace(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void debug(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void info(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void warn(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void error(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void fatal(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Fatal, std::format(fmt, std::forward<Args>(args)...));
	}
}