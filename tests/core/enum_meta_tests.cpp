#include <core/meta/EnumImpl.hpp>
#include <core/meta/EnumMapping.hpp>

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

TEST(EnumMetaTest, ProvidesReflectionAndAnchoredMapping) {
    EXPECT_EQ(core::countOf<TestEnum>(), 2u);
    EXPECT_EQ(core::indexOf(TestEnum::Second), 1u);
    EXPECT_EQ(core::toStringView(TestEnum::First), "First");
    EXPECT_EQ(core::parseEnum<TestEnum>("Second"), TestEnum::Second);
    EXPECT_FALSE(core::parseEnum<TestEnum>("Missing").has_value());

    using Mapping = core::EnumMapping<TestMappedEnum, uint32_t>;
    EXPECT_EQ(Mapping::map(TestMappedEnum::First), 10u);
    EXPECT_EQ(Mapping::map(TestMappedEnum::Second), 11u);
    EXPECT_EQ(Mapping::map(TestMappedEnum::Third), 30u);
    EXPECT_EQ(Mapping::unmap(11u), TestMappedEnum::Second);
}
