#include "Logger.hpp"
#include <iostream>
#include <syncstream>
#include <Core/Common/Timer.hpp>
#include <Core/Common/DurationToString.hpp>
#include "ConsoleColor.hpp"

core::io::LogLevel g_logLevel = core::io::LogLevel::Warn;

void core::io::setLogLevel(LogLevel level) {
	g_logLevel = level;
}

void core::io::log(LogLevel level, const std::string& msg) {
	static const char* LOG_LEVEL_STR[] = { " TRACE]: ", " DEBUG]: ", " INFO]: ", " WARN]: ", " ERROR]: ", " FATAL]: " };
	static ConsoleColor LOG_LEVEL_FOREGROUND_COLOR[] = { Gray, BrightGreen, BrightBlue, BrightYellow, BrightRed, Red };
	static ConsoleColor LOG_LEVEL_BACKGROUND_COLOR[] = { Black, Black, Black, Gray, Gray, Gray };

	auto const now = common::Timer::global().elapsed();
	std::osyncstream(std::clog) 
		<< Foreground { LOG_LEVEL_FOREGROUND_COLOR[static_cast<size_t>(level)] } 
		<< Background { LOG_LEVEL_BACKGROUND_COLOR[static_cast<size_t>(level)] } 
		<< "[" << common::durationToString(now) << LOG_LEVEL_STR[static_cast<size_t>(level)] << msg << std::endl;
}
