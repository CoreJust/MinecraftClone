#include <core/common/SpanUtils.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

template<typename T>
concept StringViewBorrowable = requires(T&& value) {
    core::asStringView(static_cast<T&&>(value));
};

} // namespace

TEST(SpanUtilsTest, ConvertsCollectionsToByteAndElementSpans) {
    std::array<uint16_t, 2> values{ 0x1234, 0x5678 };
    auto bytes = core::asByteSpan(values);
    auto elements = core::asSpan<uint16_t>(values);
    EXPECT_EQ(bytes.size(), sizeof(values));
    ASSERT_EQ(elements.size(), values.size());
    EXPECT_EQ(elements[0], values[0]);
    EXPECT_EQ(elements[1], values[1]);
}

TEST(SpanUtilsTest, ConvertsEmptyAndStringCollections) {
    std::array<uint16_t, 0> empty_values{ };
    EXPECT_TRUE(core::asByteSpan(empty_values).empty());
    EXPECT_TRUE(core::asSpan<uint16_t>(empty_values).empty());
    std::string const value = "hello";
    std::string const empty_string;
    EXPECT_EQ(core::asStringView(value), "hello");
    EXPECT_TRUE(core::asStringView(empty_string).empty());
}

TEST(SpanUtilsTest, StringViewBorrowsOriginalStringStorage) {
    static constexpr uint32_t SOURCE_LENGTH = 128;

    std::string source(SOURCE_LENGTH, 'a');
    std::string_view const view = core::asStringView(source);

    EXPECT_EQ(view.data(), source.data());
    source.front() = 'b';
    EXPECT_EQ(view.front(), 'b');
}

static_assert(StringViewBorrowable<std::string&>);
static_assert(!StringViewBorrowable<std::string>);
