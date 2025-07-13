#include "Debug.hpp"
#include "OS.hpp"
#include <Core/Macro/Compiler.hpp>
#if OS != WINDOWS && !((OS == LINUX || OS == OSX) && (COMPILER == GCC || COMPILER == CLANG))
#  include <csignal>
#endif

namespace core::os {
    void debugBreak() {
#if OS == WINDOWS
        DebugBreak();
#elif (OS == LINUX || OS == OSX) && (COMPILER == GCC || COMPILER == CLANG)
        __builtin_trap();
#else
        std::raise(SIGTRAP);
#endif
    }
} // namespace core::os
