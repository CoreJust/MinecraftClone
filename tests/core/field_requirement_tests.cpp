#include <core/meta/FieldRequirement.hpp>

#include <gtest/gtest.h>

struct FieldObject final {
    int value = 0;
};

TEST(FieldRequirementTest, ComparesFieldAgainstAllRelations) {
    FieldObject const object{ .value = 10 };
    EXPECT_TRUE(core::requireFieldEq<&FieldObject::value>(10)(object));
    EXPECT_FALSE(core::requireFieldEq<&FieldObject::value>(9)(object));
    EXPECT_TRUE(core::requireFieldLess<&FieldObject::value>(9)(object));
    EXPECT_FALSE(core::requireFieldLess<&FieldObject::value>(10)(object));
    EXPECT_TRUE(core::requireFieldLessEq<&FieldObject::value>(10)(object));
    EXPECT_FALSE(core::requireFieldLessEq<&FieldObject::value>(11)(object));
    EXPECT_TRUE(core::requireFieldGreater<&FieldObject::value>(11)(object));
    EXPECT_FALSE(core::requireFieldGreater<&FieldObject::value>(10)(object));
    EXPECT_TRUE(core::requireFieldGreaterEq<&FieldObject::value>(10)(object));
    EXPECT_FALSE(core::requireFieldGreaterEq<&FieldObject::value>(9)(object));
}
