#include <core/common/CrashHandler.hpp>

#include <csignal>
#include <cstdlib>

#ifdef _WIN32
// DONT_CHECK INCLUDE_ORDER
#include <windows.h>
#else
// DONT_CHECK INCLUDE_ORDER
#include <unistd.h>
#endif

namespace core {
namespace {

#ifndef _WIN32
constexpr char SIGNAL_TERMINATION_MESSAGE[] = "Signal received; exiting immediately without cleanup.\n";
#endif

void onErrorSignal([[maybe_unused]] int const code) noexcept {
#ifdef _WIN32
    std::_Exit(EXIT_FAILURE);
#else
    ::write(STDERR_FILENO, SIGNAL_TERMINATION_MESSAGE, sizeof(SIGNAL_TERMINATION_MESSAGE) - 1);
    std::_Exit(128 + code);
#endif
}

#ifdef _WIN32
LONG WINAPI onWindowsException(EXCEPTION_POINTERS*) noexcept {
    std::_Exit(EXIT_FAILURE);
}
#endif

} // namespace

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
