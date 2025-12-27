#pragma once

#include <iosfwd>

namespace sb {

enum class ConsoleColor {
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

    Current,
};

struct TextColor final {
    ConsoleColor background = ConsoleColor::Current;
    ConsoleColor foreground = ConsoleColor::Current;
};

void setConsoleColor(TextColor color);
[[nodiscard]]
TextColor getConsoleColor() noexcept;

constexpr TextColor foreground(ConsoleColor color) noexcept { return { .foreground = color }; }
constexpr TextColor background(ConsoleColor color) noexcept { return { .background = color }; }

inline std::ostream& operator<<(std::ostream& out, TextColor color) {
    setConsoleColor(color);
    return out;
}

} // namespace sb
