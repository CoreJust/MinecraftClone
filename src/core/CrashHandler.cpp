#include "core/CrashHandler.hpp"

#include "core/AtAppExit.hpp"
#include "core/Log.hpp"

#include <csignal>
#include <stacktrace>

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
#ifdef SIGKILL
        case SIGBUS: return "SIGKILL";
#endif
#ifdef SIGQUIT
        case SIGQUIT: return "SIGQUIT";
#endif
#ifdef SIGTSTP
        case SIGTSTP: return "SIGTSTP";
#endif
#ifdef SIGSYS
        case SIGSYS: return "SIGSYS";
#endif
    default: return "Unknown";
    }
}

extern "C" void onErrorSignal(int const code) {
    MC_CRITICAL(
        "Caught error signal: {}\nStacktrace:\n{}",
        decodeSignalCode(code),
        std::stacktrace::current());
    core::AtAppExit::exit();
    exit(1);
}

namespace core {

void setCrashHandler() {
    std::signal(SIGABRT, onErrorSignal);
    std::signal(SIGFPE,  onErrorSignal);
    std::signal(SIGILL,  onErrorSignal);
    std::signal(SIGINT,  onErrorSignal);
    std::signal(SIGSEGV, onErrorSignal);
    std::signal(SIGTERM, onErrorSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onErrorSignal);
#endif
#ifdef SIGBUS
    std::signal(SIGBUS, onErrorSignal);
#endif
#ifdef SIGKILL
    std::signal(SIGKILL, onErrorSignal);
#endif
#ifdef SIGQUIT
    std::signal(SIGQUIT, onErrorSignal);
#endif
#ifdef SIGTSTP
    std::signal(SIGTSTP, onErrorSignal);
#endif
#ifdef SIGSYS
    std::signal(SIGSYS, onErrorSignal);
#endif
}

} // namespace core
