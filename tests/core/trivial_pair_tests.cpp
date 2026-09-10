#include <core/common/TrivialPair.hpp>

#include <gtest/gtest.h>

#include <type_traits>

TEST(TrivialPairTest, StoresBothValuesAsAggregate) {
    core::TrivialPair<int, std::string> pair{ .first = 42, .second = "answer" };

    EXPECT_EQ(pair.first, 42);
    EXPECT_EQ(pair.second, "answer");
    static_assert(std::is_aggregate_v<decltype(pair)>);
}
