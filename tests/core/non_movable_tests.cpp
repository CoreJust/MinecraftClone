#include <core/common/NonMovable.hpp>

#include <gtest/gtest.h>

#include <type_traits>

namespace {

struct NonMovableValue final : core::NonMovable {
    int value = 0;
};

static_assert(std::is_copy_constructible_v<NonMovableValue>);
static_assert(std::is_copy_assignable_v<NonMovableValue>);
static_assert(!std::is_move_constructible_v<core::NonMovable>);
static_assert(!std::is_move_assignable_v<core::NonMovable>);

} // namespace

TEST(NonMovableTest, CopiesFromConstSource) {
    NonMovableValue const original{ .value = 42 };
    NonMovableValue copy{ original };
    EXPECT_EQ(copy.value, 42);
}

TEST(NonMovableTest, AssignsFromConstSource) {
    NonMovableValue const original{ .value = 42 };
    NonMovableValue copy{ .value = 0 };
    copy = original;
    EXPECT_EQ(copy.value, 42);
}
