#include <core/common/VectorUtils.hpp>

#include <gtest/gtest.h>

#include <array>
#include <vector>

TEST(VectorUtilsTest, AppendsRangeAndReturnsDestination) {
    std::vector<int> values{ 1 };
    std::array<int, 2> extra{ 2, 3 };
    auto& result = core::appendRange(values, extra);
    EXPECT_EQ(&result, &values);
    EXPECT_EQ(values, (std::vector{ 1, 2, 3 }));
}

TEST(VectorUtilsTest, AppendsEmptyRangeWithoutChangingDestination) {
    std::vector<int> values{ 1, 2 };
    std::array<int, 0> extra{ };
    core::appendRange(values, extra);
    EXPECT_EQ(values, (std::vector{ 1, 2 }));
}
