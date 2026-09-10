#include <core/common/ByteReader.hpp>
#include <core/common/ByteWriter.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Round-trip a single scalar through ByteWriter -> ByteReader.
template<typename T>
void roundTripScalar(T const value) {
    core::ByteWriter writer;
    writer.write(value);
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    auto const read = reader.read<T>();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, value);
    EXPECT_EQ(reader.left(), 0u);
}

} // namespace

TEST(ByteIOTest, RoundTripsScalarTypes) {
    roundTripScalar<uint8_t>(0xABu);
    roundTripScalar<uint32_t>(0xDEAD'BEEFu);
    roundTripScalar<int64_t>(-1'000'000'000'000);
    roundTripScalar<float>(3.141'59f);
    roundTripScalar<double>(2.718'281'828);
    roundTripScalar<char>('Q');
}

TEST(ByteIOTest, RoundTripMultipleValuesInSequence) {
    core::ByteWriter writer;
    writer
        .write(uint8_t{ 1 })
        .write(uint32_t{ 2 })
        .write(int16_t{ -3 })
        .write('c')
    ;
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    EXPECT_EQ(*reader.read<uint8_t>(), 1u);
    EXPECT_EQ(*reader.read<uint32_t>(), 2u);
    EXPECT_EQ(*reader.read<int16_t>(), -3);
    EXPECT_EQ(*reader.read<char>(), 'c');
    EXPECT_FALSE(reader.read<char>().has_value());
    EXPECT_EQ(reader.left(), 0u);
}

TEST(ByteIOTest, SpanRoundTripPreservesElementCount) {
    std::array<uint32_t const, 4> const src{ 10, 20, 30, 40 };
    core::ByteWriter writer;
    writer.write(std::span{ src });
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    auto const read = reader.readSpan<uint32_t>();
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->size(), src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_EQ((*read)[i], src[i]);
    }
    EXPECT_EQ(reader.left(), 0u);
}

TEST(ByteIOTest, SpanRoundTripWorksWithSingleByteElements) {
    std::vector<uint8_t> const src{ 1, 2, 3, 4, 5 };
    core::ByteWriter writer;
    writer.write(std::span{ src });
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    auto const read = reader.readSpan<uint8_t>();
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->size(), src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_EQ((*read)[i], src[i]);
    }
}

TEST(ByteIOTest, StringRoundTrip) {
    std::string const s = "hello, world";
    core::ByteWriter writer;
    writer.write(std::string_view{ s });
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    auto const read = reader.readStr();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, s);
    EXPECT_EQ(reader.left(), 0u);
}

TEST(ByteIOTest, TruncatedScalarReturnsNullopt) {
    std::array<uint8_t, 1> bytes{ 0x42 };
    core::ByteReader reader{ bytes };
    EXPECT_FALSE(reader.read<uint32_t>().has_value());
    EXPECT_EQ(reader.pos(), 0u);
}

TEST(ByteIOTest, TruncatedSpanLengthPrefixReturnsNullopt) {
    std::array<uint8_t, 3> bytes{ 1, 2, 3 };
    core::ByteReader reader{ bytes };
    EXPECT_FALSE(reader.readSpan<uint32_t>().has_value());
    EXPECT_EQ(reader.pos(), 0u);
}

TEST(ByteIOTest, OversizedSpanLengthReturnsNullopt) {
    core::ByteWriter writer;
    writer.write(uint64_t{ 1ULL << 40 });
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    EXPECT_FALSE(reader.readSpan<uint32_t>().has_value());
}

TEST(ByteIOTest, OverflowSafeSpanBoundDoesNotWrap) {
    core::ByteWriter writer;
    writer.write(uint64_t{ (static_cast<uint64_t>(-1)) / sizeof(uint32_t) + 1 });
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    EXPECT_FALSE(reader.readSpan<uint32_t>().has_value());
}

TEST(ByteIOTest, RoundTripEmptySpan) {
    std::span<uint32_t const> const empty;
    core::ByteWriter writer;
    writer.write(empty);
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    auto const read = reader.readSpan<uint32_t>();
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read->empty());
    EXPECT_EQ(reader.left(), 0u);
}

TEST(ByteIOTest, WriterRespectsReserveCapacity) {
    core::ByteWriter writer;
    writer.reserve(1024);
    EXPECT_GE(writer.size(), 0u);
    writer.write(uint64_t{ 0 });
    EXPECT_EQ(writer.size(), sizeof(uint64_t));
}

TEST(ByteIOTest, SpanAndScalarInterleaveRoundTrip) {
    std::array<int32_t const, 3> const span_data{ 100, 200, 300 };
    core::ByteWriter writer;
    writer
        .write(uint8_t{ 7 })
        .write(std::span{ span_data })
        .write(uint16_t{ 9 })
    ;
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    EXPECT_EQ(*reader.read<uint8_t>(), 7u);
    auto const span = reader.readSpan<int32_t>();
    ASSERT_TRUE(span.has_value());
    ASSERT_EQ(span->size(), span_data.size());
    for (size_t i = 0; i < span_data.size(); ++i) {
        EXPECT_EQ((*span)[i], span_data[i]);
    }
    EXPECT_EQ(*reader.read<uint16_t>(), 9u);
    EXPECT_EQ(reader.left(), 0u);
}

TEST(ByteIOTest, MixedValueAndStringRoundTrip) {
    std::string const name = "player-one";
    core::ByteWriter writer;
    writer
        .write(uint32_t{ 42 })
        .write(std::string_view{ name })
        .write(uint8_t{ 3 })
    ;
    std::vector<uint8_t> const bytes = writer.build();

    core::ByteReader reader{ bytes };
    EXPECT_EQ(*reader.read<uint32_t>(), 42u);
    auto const s = reader.readStr();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, name);
    EXPECT_EQ(*reader.read<uint8_t>(), 3u);
    EXPECT_EQ(reader.left(), 0u);
}
