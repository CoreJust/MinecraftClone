#include <core/common/Assert.hpp>
#include <core/meta/Enum.hpp>
#include <core/meta/EnumImpl.hpp>
#include <core/meta/TypeName.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

enum class TypeNameTestEnum : uint8_t {
    First,
    Count,
};

CORE_ENUM_FUNCTIONS(TypeNameTestEnum);
CORE_ENUM_FUNCTIONS_IMPL(TypeNameTestEnum);

struct TypeNameTestObject final { };

TEST(TypeNameTest, ExtractsTypeAndValueNames) {
    EXPECT_EQ(core::typeName<TypeNameTestObject>(), "TypeNameTestObject");
    EXPECT_EQ(core::valueName<TypeNameTestEnum::First>(), "First");
}
