#include <core/common/InputSpan.hpp>

#include <gtest/gtest.h>

#include <array>
#include <vector>

TEST(InputSpanTest, AcceptsArrayAndSpan) {
    auto const copy = [](core::InputSpan<int> const input) {
        return std::vector<int>(input.begin(), input.end());
    };
    std::array<int, 3> values{ 1, 2, 3 };
    core::InputSpan<int> from_array{ values };
    core::InputSpan<int> from_span{ std::span{ values } };
    int copied_values[] = { 4, 5, 6 };
    EXPECT_EQ(std::vector<int>(from_array.begin(), from_array.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_span.begin(), from_span.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(copy(copied_values), (std::vector{ 4, 5, 6 }));
}

TEST(InputSpanTest, PreservesEmptyRange) {
    std::vector<int> values;
    core::InputSpan<int> input{ std::span{ values } };
    EXPECT_EQ(input.begin(), input.end());
    EXPECT_EQ(std::distance(input.begin(), input.end()), 0);
}
