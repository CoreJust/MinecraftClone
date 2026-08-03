#include <core/common/InputSpan.hpp>
#include <core/common/SpanUtils.hpp>
#include <core/common/VectorUtils.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

TEST(RangeSpanTest, InputSpanAcceptsArraySpanAndInitializerList) {
    std::array<int, 3> values{ 1, 2, 3 };
    core::InputSpan<int> from_array{ values };
    core::InputSpan<int> from_span{ std::span{ values } };
    core::InputSpan<int> from_list{ { 4, 5, 6 } };
    EXPECT_EQ(std::vector<int>(from_array.begin(), from_array.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_span.begin(), from_span.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_list.begin(), from_list.end()), (std::vector{ 4, 5, 6 }));
}

TEST(RangeSpanTest, InputSpanPreservesEmptyRange) {
    std::vector<int> values;
    core::InputSpan<int> input{ std::span{ values } };
    EXPECT_EQ(input.begin(), input.end());
    EXPECT_EQ(std::distance(input.begin(), input.end()), 0);
}

TEST(RangeSpanTest, AppendsRangesAndPreservesDestinationForEmptyRange) {
    std::vector<int> values{ 1 };
    std::array<int, 2> extra{ 2, 3 };
    auto& result = core::appendRange(values, extra);
    EXPECT_EQ(&result, &values);
    EXPECT_EQ(values, (std::vector{ 1, 2, 3 }));
    std::array<int, 0> empty{ };
    core::appendRange(values, empty);
    EXPECT_EQ(values, (std::vector{ 1, 2, 3 }));
}

TEST(RangeSpanTest, ConvertsCollectionsToByteAndElementSpans) {
    std::array<uint16_t, 2> values{ 0x1234, 0x5678 };
    auto bytes = core::asByteSpan(values);
    auto elements = core::asSpan<uint16_t>(bytes);
    EXPECT_EQ(bytes.size(), sizeof(values));
    ASSERT_EQ(elements.size(), values.size());
    EXPECT_EQ(elements[0], values[0]);
    EXPECT_EQ(elements[1], values[1]);
}

TEST(RangeSpanTest, ConvertsEmptyAndStringCollections) {
    std::array<uint16_t, 0> empty_values{ };
    EXPECT_TRUE(core::asByteSpan(empty_values).empty());
    EXPECT_TRUE(core::asSpan<uint16_t>(empty_values).empty());
    std::string const value = "hello";
    std::string const empty_string;
    EXPECT_EQ(core::asStringView(value), "hello");
    EXPECT_TRUE(core::asStringView(empty_string).empty());
}
