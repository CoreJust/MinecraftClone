#include <core/common/StaticInitializer.hpp>

#include <gtest/gtest.h>

namespace {

struct Initializable final {
    static inline int init_count = 0;
    static inline int destroy_count = 0;

    static void init() { ++init_count; }
    static void destroy() { ++destroy_count; }
};

} // namespace

TEST(StaticInitializerTest, InitializesOnce) {
    using Initializer = core::StaticInitializer<Initializable>;
    Initializer::ensureInit();
    Initializer::ensureInit();

    EXPECT_TRUE(Initializer::isInitialized());
    EXPECT_EQ(Initializable::init_count, 1);
}
