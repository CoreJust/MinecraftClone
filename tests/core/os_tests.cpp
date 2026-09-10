#include <core/macro/OS.hpp>

#include <gtest/gtest.h>

TEST(OSTest, IdentifiesTheCurrentPlatform) {
#if defined(OSX)
    SUCCEED();
#elif defined(WINDOWS)
    SUCCEED();
#else
    FAIL() << "Current platform is not supported";
#endif
}
