#include <core/common/CrashHandler.hpp>

#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>

namespace {

#ifdef _WIN32
static constexpr int32_t SIGNAL_EXIT_CODE = EXIT_FAILURE;
static constexpr char SIGNAL_OUTPUT[] = "";
#else
static constexpr int32_t SIGNAL_EXIT_CODE_OFFSET = 128;
static constexpr char SIGNAL_OUTPUT[] = "Signal received; exiting immediately without cleanup.";
#endif

int32_t expectedExitCode(int32_t const signal_code)
{
#ifdef _WIN32
    return SIGNAL_EXIT_CODE;
#else
    return SIGNAL_EXIT_CODE_OFFSET + signal_code;
#endif
}

void raiseSignal(int32_t const signal_code)
{
    core::setCrashHandler();
    std::raise(signal_code);
}

} // namespace

TEST(CrashHandlerTest, TerminatesForInteractiveSignals)
{
    static constexpr int32_t INTERRUPT_SIGNAL = SIGINT;
    static constexpr int32_t TERMINATION_SIGNAL = SIGTERM;

    ASSERT_EXIT(
        raiseSignal(INTERRUPT_SIGNAL),
        ::testing::ExitedWithCode(expectedExitCode(INTERRUPT_SIGNAL)),
        SIGNAL_OUTPUT
    );
    ASSERT_EXIT(
        raiseSignal(TERMINATION_SIGNAL),
        ::testing::ExitedWithCode(expectedExitCode(TERMINATION_SIGNAL)),
        SIGNAL_OUTPUT
    );
}

TEST(CrashHandlerTest, TerminatesForFatalSignal)
{
    static constexpr int32_t FATAL_SIGNAL = SIGABRT;

    ASSERT_EXIT(
        raiseSignal(FATAL_SIGNAL),
        ::testing::ExitedWithCode(expectedExitCode(FATAL_SIGNAL)),
        SIGNAL_OUTPUT
    );
}
