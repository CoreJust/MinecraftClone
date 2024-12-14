#include "ConsoleColor.hpp"

core::io::ConsoleColor g_currentForegroundColor = core::io::White;
core::io::ConsoleColor g_currentBackgroundColor = core::io::Black;

#ifdef _WIN32
#include <Windows.h>

void setOutputColor() {
    auto const stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD const color = (static_cast<WORD>(g_currentBackgroundColor) << 4) | static_cast<WORD>(g_currentForegroundColor);
    FlushConsoleInputBuffer(stdHandle);
    SetConsoleTextAttribute(stdHandle, color);
}

#else // Linux
#include <iostream>

void setOutputColor() {
    std::cout << "\033[" << static_cast<int>(g_currentForegroundColor) << ';' << (static_cast<int>(g_currentBackgroundColor) + 10) << 'm';
}
#endif

std::ostream& core::io::operator<<(std::ostream& out, ConsoleColor color) {
    if (g_currentForegroundColor != color) {
        g_currentForegroundColor = color;
        setOutputColor();
    }
    return out;
}

std::ostream& core::io::operator<<(std::ostream& out, Background color) {
    if (g_currentBackgroundColor != color.color) {
        g_currentBackgroundColor = color.color;
        setOutputColor();
    }
    return out;
}

std::ostream& core::io::operator<<(std::ostream& out, Foreground color) {
    if (g_currentForegroundColor != color.color) {
        g_currentForegroundColor = color.color;
        setOutputColor();
    }
    return out;
}