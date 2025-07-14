#pragma once
#include <ostream>
#include <Core/Common/Int.hpp>

namespace core::io {
#ifdef _WIN32
	enum ConsoleColor : u8 {
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
	enum ConsoleColor : u8 {
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
