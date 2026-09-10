#include <core/common/Defer.hpp>

#include <gtest/gtest.h>

#include <vector>

TEST(DeferTest, RunsAtScopeExit) {
    int calls = 0;
    {
        defer { ++calls; };
        EXPECT_EQ(calls, 0);
    }
    EXPECT_EQ(calls, 1);
}

TEST(DeferTest, RunsInReverseDeclarationOrder) {
    std::vector<int> order;
    {
        defer { order.push_back(1); };
        defer { order.push_back(2); };
    }
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 1);
}

TEST(DeferTest, CapturesReferencesAtScopeExit) {
    int value = 10;
    {
        defer { value *= 2; };
        value += 5;
    }
    EXPECT_EQ(value, 30);
}
