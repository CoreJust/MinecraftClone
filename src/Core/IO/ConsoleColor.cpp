#include "ConsoleColor.hpp"

#ifdef _WIN32
#include <Windows.h>
#else // Linux
#include <iostream>
#endif

namespace core::io {
    ConsoleColor g_currentForegroundColor = White;
    ConsoleColor g_currentBackgroundColor = Black;

namespace {
    void setOutputColor() {
#ifdef _WIN32
        auto const stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        WORD const color = static_cast<WORD>((static_cast<WORD>(g_currentBackgroundColor) << 4) | static_cast<WORD>(g_currentForegroundColor));
        FlushConsoleInputBuffer(stdHandle);
        SetConsoleTextAttribute(stdHandle, color);
#else // Linux
        std::cout << "\033[" << static_cast<int>(g_currentForegroundColor) << ';' << (static_cast<int>(g_currentBackgroundColor) + 10) << 'm';
#endif
    }
} // namespace

    std::ostream& operator<<(std::ostream& out, ConsoleColor color) {
        if (g_currentForegroundColor != color) {
            g_currentForegroundColor = color;
            setOutputColor();
        }
        return out;
    }

    std::ostream& operator<<(std::ostream& out, Background color) {
        if (g_currentBackgroundColor != color.color) {
            g_currentBackgroundColor = color.color;
            setOutputColor();
        }
        return out;
    }

    std::ostream& operator<<(std::ostream& out, Foreground color) {
        if (g_currentForegroundColor != color.color) {
            g_currentForegroundColor = color.color;
            setOutputColor();
        }
        return out;
    }
} // namespace core::io
