#include "SignalHandler.hpp"
#include <csignal>
#include <stacktrace>
#include <Core/IO/Logger.hpp>

extern "C" char const* decodeSignalCode(int const code) {
    switch (code) {
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGINT:  return "SIGINT";
        case SIGSEGV: return "SIGSEGV";
        case SIGTERM: return "SIGTERM";
    default: return "Unknown";
    }
}

extern "C" void onErrorSignal(int const code) {
	core::io::error(
		"Caught error signal: {}\nStacktrace:\n{}",
		decodeSignalCode(code),
		std::stacktrace::current());
}

namespace core::os {
    void setErrorSignalHandlers() {
        std::signal(SIGABRT, ::onErrorSignal);
        std::signal(SIGFPE,  ::onErrorSignal);
        std::signal(SIGILL,  ::onErrorSignal);
        std::signal(SIGSEGV, ::onErrorSignal);
        std::signal(SIGTERM, ::onErrorSignal);
    }
} // namespace core::os
