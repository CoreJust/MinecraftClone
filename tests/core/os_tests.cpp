#if !defined(__linux__) || defined(__ANDROID__)
#include <core/macro/OS.hpp>
#endif

#include <gtest/gtest.h>

TEST(OSTest, IdentifiesTheCurrentPlatform) {
#if defined(__linux__) && !defined(__ANDROID__)
    GTEST_SKIP() << "Linux is an analysis-only host, not a supported product platform";
#elif defined(OSX)
    SUCCEED();
#elif defined(WINDOWS)
    SUCCEED();
#else
    FAIL() << "Current platform is not supported";
#endif
}
