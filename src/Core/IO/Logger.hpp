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

	void setLogLevel(LogLevel const level) noexcept; // Not thread safe, must be called from a single thread at a time
	LogLevel getLogLevel() noexcept;
	void onLoggingEnd();
	void log(LogLevel const level, std::string const& msg);

	template<class... Args>
	void trace(std::format_string<Args...> fmt, Args&&... args) {
		if (static_cast<int>(getLogLevel()) == static_cast<int>(LogLevel::Trace))
			log(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void debug(std::format_string<Args...> fmt, Args&&... args) {
		if (static_cast<int>(getLogLevel()) <= static_cast<int>(LogLevel::Debug))
			log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void info(std::format_string<Args...> fmt, Args&&... args) {
		if (static_cast<int>(getLogLevel()) <= static_cast<int>(LogLevel::Info))
			log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void warn(std::format_string<Args...> fmt, Args&&... args) {
		if (static_cast<int>(getLogLevel()) <= static_cast<int>(LogLevel::Warn))
			log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void error(std::format_string<Args...> fmt, Args&&... args) {
		if (static_cast<int>(getLogLevel()) <= static_cast<int>(LogLevel::Error))
			log(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
	}

	template<class... Args>
	void fatal(std::format_string<Args...> fmt, Args&&... args) {
		log(LogLevel::Fatal, std::format(fmt, std::forward<Args>(args)...));
	}
} // namespace core::io
