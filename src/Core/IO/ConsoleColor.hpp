#pragma once
#include <cstdint>
#include <ostream>

namespace core::io {
#ifdef _WIN32
	enum ConsoleColor : uint8_t {
		Black,
		Blue,
		Green,
		Cyan,
		Red,
		Magenta,
		Yellow,
		LightGray,
		Gray,
		BrightBlue,
		BrightGreen,
		BrightCyan,
		BrightRed,
		BrightMagenta,
		BrightYellow,
		White,
	};
#else // Linux
	enum ConsoleColor : uint8_t {
		Black = 30,
		Red,
		Green,
		Yellow,
		Blue,
		Magenta,
		Cyan,
		LightGray,
		Gray = 90,
		BrightRed,
		BrightGreen,
		BrightYellow,
		BrightBlue,
		BrightMagenta,
		BrightCyan,
		White,
	};
#endif

	struct Background final {
		ConsoleColor color;
	};

	struct Foreground final {
		ConsoleColor color;
	};

	std::ostream& operator<<(std::ostream& out, ConsoleColor color);
	std::ostream& operator<<(std::ostream& out, Background color);
	std::ostream& operator<<(std::ostream& out, Foreground color);
} // namespace core::io
