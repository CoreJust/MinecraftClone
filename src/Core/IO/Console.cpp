#include "Console.hpp"
#include <Core/OS/OS.hpp>

namespace core::io {
    void enableUtf8Cout() {
#if OS == WINDOWS
        SetConsoleOutputCP(CP_UTF8);
#endif
    }
} // namespace core::io
