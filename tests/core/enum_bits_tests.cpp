#include <core/common/EnumBits.hpp>

#include <gtest/gtest.h>

enum class TestBit {
    First,
    Second,
    Third,
    Count,
};

TEST(EnumBitsTest, CreatesAndQueriesIndividualBits) {
    using Bits = core::EnumBits<TestBit>;

    Bits const first{ TestBit::First };

    EXPECT_TRUE(first[TestBit::First]);
    EXPECT_FALSE(first[TestBit::Second]);
    EXPECT_FALSE(first[TestBit::Third]);
}

TEST(EnumBitsTest, CombinesBitsWithFactoryAndOperators) {
    using Bits = core::EnumBits<TestBit>;

    Bits bits = Bits::of(TestBit::First, TestBit::Third);
    bits |= TestBit::Second;
    bits |= Bits::of(TestBit::First);

    EXPECT_TRUE(bits[TestBit::First]);
    EXPECT_TRUE(bits[TestBit::Second]);
    EXPECT_TRUE(bits[TestBit::Third]);
    EXPECT_EQ(bits.value, 7u);
}

TEST(EnumBitsTest, SupportsWiderUnderlyingType) {
    enum class WideBit {
        First = 0,
        Last = 63,
        Count = 64,
    };

    using Bits = core::EnumBits<WideBit, uint64_t>;
    Bits const bits = Bits::of(WideBit::First, WideBit::Last);

    EXPECT_TRUE(bits[WideBit::First]);
    EXPECT_TRUE(bits[WideBit::Last]);
    EXPECT_EQ(bits.value, (uint64_t{ 1 } << 63) | 1u);
}
