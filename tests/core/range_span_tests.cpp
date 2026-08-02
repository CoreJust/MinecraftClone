#include <core/common/InputSpan.hpp>
#include <core/common/SpanUtils.hpp>
#include <core/common/VectorUtils.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

TEST(InputSpanTest, AcceptsInitializerListArrayAndSpan) {
    std::array<int, 3> values{ 1, 2, 3 };

    core::InputSpan<int> from_array{ values };
    core::InputSpan<int> from_span{ std::span{ values } };
    core::InputSpan<int> from_list{ { 4, 5, 6 } };

    EXPECT_EQ(std::vector<int>(from_array.begin(), from_array.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_span.begin(), from_span.end()), (std::vector{ 1, 2, 3 }));
    EXPECT_EQ(std::vector<int>(from_list.begin(), from_list.end()), (std::vector{ 4, 5, 6 }));
}

TEST(VectorUtilsTest, AppendsRangeAndReturnsDestination) {
    std::vector<int> values{ 1 };
    std::array<int, 2> extra{ 2, 3 };

    auto& result = core::appendRange(values, extra);

    EXPECT_EQ(&result, &values);
    EXPECT_EQ(values, (std::vector{ 1, 2, 3 }));
}

TEST(SpanUtilsTest, ConvertsCollectionsToByteAndElementSpans) {
    std::array<uint16_t, 2> values{ 0x1234, 0x5678 };

    auto bytes = core::asByteSpan(values);
    auto elements = core::asSpan<uint16_t>(bytes);

    EXPECT_EQ(bytes.size(), sizeof(values));
    ASSERT_EQ(elements.size(), values.size());
    EXPECT_EQ(elements[0], values[0]);
    EXPECT_EQ(elements[1], values[1]);
}

TEST(SpanUtilsTest, ConvertsStringLikeCollectionsToStringView) {
    std::string const value = "hello";

    EXPECT_EQ(core::asStringView(value), "hello");
}
