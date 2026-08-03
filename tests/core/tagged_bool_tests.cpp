#include <core/meta/TaggedBool.hpp>

#include <gtest/gtest.h>

struct TestTag;

TEST(TaggedBoolTest, ProvidesTypedValuesAndConversions) {
    using Bool = core::TaggedBool<TestTag>;
    EXPECT_TRUE(static_cast<bool>(Bool::Yes));
    EXPECT_FALSE(static_cast<bool>(Bool::No));
    EXPECT_EQ(Bool::Yes.toStringView(), "Yes");
    EXPECT_EQ(Bool::No.toStringView(), "No");
    EXPECT_EQ(core::alias_cast<Bool>(Bool::Yes), Bool::Yes);
    EXPECT_NE(Bool::Yes, Bool::No);
}
