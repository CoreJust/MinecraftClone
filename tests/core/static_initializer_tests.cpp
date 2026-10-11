#include <core/common/StaticInitializer.hpp>

#include <gtest/gtest.h>

#include <string>

namespace {

struct Initializable final {
    static inline int init_count = 0;

    static void init() { ++init_count; }
    static void destroy() {}
};

} // namespace

TEST(StaticInitializerTest, InitializesOnce) {
    using Initializer = core::StaticInitializer<Initializable>;
    Initializer::ensureInit();
    Initializer::ensureInit();

    EXPECT_TRUE(Initializer::isInitialized());
    EXPECT_EQ(Initializable::init_count, 1);
}
