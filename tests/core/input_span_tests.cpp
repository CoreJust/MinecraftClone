#include <core/common/InputSpan.hpp>

#include <gtest/gtest.h>

#include <array>
#include <vector>

TEST(InputSpanTest, AcceptsArraySpanAndInitializerList) {
    std::array<int, 3> values{ 1, 2, 3 };
    core::InputSpan<int> from_array{ values };
    core::InputSpan<int> from_span{ std::span{ values } };
    core::InputSpan<int> from_list{ { 4, 5, 6 } };
    EXPECT_EQ(std::vector<int>(from_array.begin(), from_array.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_span.begin(), from_span.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_list.begin(), from_list.end()), (std::vector{ 4, 5, 6 }));
}

TEST(InputSpanTest, PreservesEmptyRange) {
    std::vector<int> values;
    core::InputSpan<int> input{ std::span{ values } };
    EXPECT_EQ(input.begin(), input.end());
    EXPECT_EQ(std::distance(input.begin(), input.end()), 0);
}
