#include <core/meta/EnumImpl.hpp>
#include <core/meta/EnumMapping.hpp>
#include <core/meta/FieldRequirement.hpp>
#include <core/meta/TaggedBool.hpp>
#include <core/meta/TypeName.hpp>

#include <gtest/gtest.h>

enum class TestEnum {
    First,
    Second,
    Count,
};

CORE_ENUM_FUNCTIONS(TestEnum);
CORE_ENUM_FUNCTIONS_IMPL(TestEnum);

enum class TestMappedEnum {
    First,
    Second,
    Third,
};

CORE_DEFINE_ENUM_MAPPING(TestMappedEnum, uint32_t,
    { TestMappedEnum::First, 10 },
    { TestMappedEnum::Third, 30 }
)

struct FieldObject final {
    int value = 0;
};

struct TestTag;

TEST(MetaEnumTest, ProvidesValuesNamesAndParsing) {
    EXPECT_EQ(core::countOf<TestEnum>(), 2u);
    EXPECT_EQ(core::indexOf(TestEnum::Second), 1u);
    EXPECT_EQ(core::toStringView(TestEnum::First), "First");
    EXPECT_EQ(core::parseEnum<TestEnum>("Second"), TestEnum::Second);
    EXPECT_FALSE(core::parseEnum<TestEnum>("Missing").has_value());
}

TEST(MetaEnumTest, MapsAnchoredEnumValues) {
    using Mapping = core::EnumMapping<TestMappedEnum, uint32_t>;

    EXPECT_EQ(Mapping::map(TestMappedEnum::First), 10u);
    EXPECT_EQ(Mapping::map(TestMappedEnum::Second), 11u);
    EXPECT_EQ(Mapping::map(TestMappedEnum::Third), 30u);
    EXPECT_EQ(Mapping::unmap(11u), TestMappedEnum::Second);
}

TEST(MetaFieldTest, BuildsFieldComparisons) {
    FieldObject object{ .value = 10 };

    EXPECT_TRUE(core::requireFieldEq<&FieldObject::value>(10)(object));
    EXPECT_TRUE(core::requireFieldLess<&FieldObject::value>(9)(object));
    EXPECT_TRUE(core::requireFieldLessEq<&FieldObject::value>(10)(object));
    EXPECT_TRUE(core::requireFieldGreater<&FieldObject::value>(11)(object));
    EXPECT_TRUE(core::requireFieldGreaterEq<&FieldObject::value>(10)(object));
    EXPECT_FALSE(core::requireFieldLess<&FieldObject::value>(10)(object));
}

TEST(MetaTaggedBoolTest, UsesStronglyTypedBooleanValues) {
    using Bool = core::TaggedBool<TestTag>;

    EXPECT_TRUE(static_cast<bool>(Bool::Yes));
    EXPECT_FALSE(static_cast<bool>(Bool::No));
    EXPECT_EQ(Bool::Yes.toStringView(), "Yes");
    EXPECT_EQ(Bool::No.toStringView(), "No");
    EXPECT_EQ(core::alias_cast<Bool>(Bool::Yes), Bool::Yes);
}

TEST(MetaTypeNameTest, ExtractsTypeAndValueNames) {
    EXPECT_EQ(core::typeName<FieldObject>(), "FieldObject");
    EXPECT_EQ(core::valueName<TestEnum::First>(), "First");
}
