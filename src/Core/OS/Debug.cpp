#include "Debug.hpp"
#include <Core/Macro/OS.hpp>
#include <Core/Macro/Compiler.hpp>
#if WINDOWS
#  include <Windows.h>
#elif !((LINUX || OSX) && (GCC || CLANG))
#  include <csignal>
#endif

namespace core::os {
    void debugBreak() {
#if WINDOWS
        DebugBreak();
#elif (LINUX || OSX) && (GCC || CLANG)
        __builtin_trap();
#else
        std::raise(SIGTRAP);
#endif
    }
} // namespace core::os
