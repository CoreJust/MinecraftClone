#include "Console.hpp"
#include <Core/Macro/OS.hpp>
#if WINDOWS
#  include <WIndows.h>
#endif

namespace core {
    void enableUtf8Cout() {
#if WINDOWS
        SetConsoleOutputCP(CP_UTF8);
#endif
    }
} // namespace core
