#include <core/common/CrashHandler.hpp>

#include <core/IO/Log.hpp>

#include <csignal>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace core {
namespace {

char const* decodeSignalCode(int const code) noexcept {
    switch (code) {
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGINT:  return "SIGINT";
        case SIGSEGV: return "SIGSEGV";
        case SIGTERM: return "SIGTERM";
#ifdef SIGBREAK
        case SIGBREAK: return "SIGBREAK";
#endif
#ifdef SIGBUS
        case SIGBUS: return "SIGBUS";
#endif
#ifdef SIGQUIT
        case SIGQUIT: return "SIGQUIT";
#endif
#ifdef SIGSYS
        case SIGSYS: return "SIGSYS";
#endif
    default: return "Unknown";
    }
}

void onErrorSignal(int const code) {
    CORE_CRITICAL(
        "Caught error signal: {}\nStacktrace:\n{}",
        decodeSignalCode(code),
        currentStacktrace());
    std::quick_exit(1);
}

#ifdef _WIN32
LONG WINAPI onWindowsException(EXCEPTION_POINTERS*) {
    CORE_CRITICAL("Caught Windows exception\nStacktrace:\n{}", currentStacktrace());
    std::quick_exit(1);
}
#endif

} // namespace

void setCrashHandler() {
    std::signal(SIGABRT, onErrorSignal);
    std::signal(SIGFPE,  onErrorSignal);
    std::signal(SIGILL,  onErrorSignal);
    std::signal(SIGSEGV, onErrorSignal);
    std::signal(SIGTERM, onErrorSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onErrorSignal);
#endif
#ifdef SIGBUS
    std::signal(SIGBUS, onErrorSignal);
#endif
#ifdef SIGQUIT
    std::signal(SIGQUIT, onErrorSignal);
#endif
#ifdef SIGSYS
    std::signal(SIGSYS, onErrorSignal);
#endif
#ifdef _WIN32
    SetUnhandledExceptionFilter(onWindowsException);
#endif
}

} // namespace core
