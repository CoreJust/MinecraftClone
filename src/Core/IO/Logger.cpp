#include "Logger.hpp"
#include <iostream>
#include <syncstream>
#include <stacktrace>
#include <Core/Common/Timer.hpp>
#include <Core/Common/DurationToString.hpp>
#include "ConsoleColor.hpp"

namespace core {
	LogLevel g_logLevel = LogLevel::Warn;

	void setLogLevel(LogLevel const level) noexcept {
		g_logLevel = level;
	}

	LogLevel getLogLevel() noexcept {
		return g_logLevel;
	}

	void onLoggingEnd() {
		std::osyncstream(std::clog) 
			<< std::flush
			<< Foreground { ConsoleColor::White } 
			<< Background { ConsoleColor::Black };
	}

	void log(LogLevel const level, std::string const& msg) {
		static const char*  LOG_LEVEL_STR[]              = { " TRACE]: ", " DEBUG]: ", " INFO]: ", " WARN]: ",   " ERROR]: ", " FATAL]: " };
		static ConsoleColor LOG_LEVEL_FOREGROUND_COLOR[] = { Gray,        BrightGreen, BrightBlue, BrightYellow, BrightRed,   Red  };
		static ConsoleColor LOG_LEVEL_BACKGROUND_COLOR[] = { Black,       Black,       Black,      Black,        Black,       Black };

		auto const now = Timer::global().elapsed();
		if (level == LogLevel::Fatal) {
			std::osyncstream(std::clog) 
				<< Foreground { LOG_LEVEL_FOREGROUND_COLOR[static_cast<usize>(level)] } 
				<< Background { LOG_LEVEL_BACKGROUND_COLOR[static_cast<usize>(level)] } 
				<< "[" << durationToString(now) << LOG_LEVEL_STR[static_cast<usize>(level)] << msg
				<< "\nStacktrace: \n" << std::stacktrace::current() << std::endl;
		} else {
			std::osyncstream(std::clog) 
				<< Foreground { LOG_LEVEL_FOREGROUND_COLOR[static_cast<usize>(level)] } 
				<< Background { LOG_LEVEL_BACKGROUND_COLOR[static_cast<usize>(level)] } 
				<< "[" << durationToString(now) << LOG_LEVEL_STR[static_cast<usize>(level)] << msg << std::endl;
		}
	}
} // namespace core
