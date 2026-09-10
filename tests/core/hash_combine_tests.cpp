#include <core/common/HashCombine.hpp>

#include <gtest/gtest.h>

TEST(HashCombineTest, DefaultHashIsStable) {
    core::HashCombiner first;
    core::HashCombiner second;

    EXPECT_EQ(first.hash(), second.hash());
}

TEST(HashCombineTest, ConsumingValuesChangesHash) {
    core::HashCombiner combiner;
    size_t const initial = combiner.hash();

    combiner.consume(42u);

    EXPECT_NE(combiner.hash(), initial);
}

TEST(HashCombineTest, SameValuesProduceSameHash) {
    core::HashCombiner first;
    core::HashCombiner second;

    first.consume(1u, std::string_view{ "two" }, 3u);
    second.consume(1u, std::string_view{ "two" }, 3u);

    EXPECT_EQ(first.hash(), second.hash());
}

TEST(HashCombineTest, OrderAffectsHash) {
    core::HashCombiner first;
    core::HashCombiner second;

    first.consume(1u, 2u);
    second.consume(2u, 1u);

    EXPECT_NE(first.hash(), second.hash());
}
