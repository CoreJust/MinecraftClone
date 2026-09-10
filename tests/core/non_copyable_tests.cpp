#include <core/common/NonCopyable.hpp>

#include <gtest/gtest.h>

#include <type_traits>

namespace {

struct NonCopyableValue final : core::NonCopyable {
    int value = 0;
};

static_assert(!std::is_copy_constructible_v<NonCopyableValue>);
static_assert(!std::is_copy_assignable_v<NonCopyableValue>);
static_assert(std::is_move_constructible_v<NonCopyableValue>);
static_assert(std::is_move_assignable_v<NonCopyableValue>);

} // namespace

TEST(NonCopyableTest, MovesDerivedValue) {
    NonCopyableValue original{ .value = 42 };
    NonCopyableValue moved{ std::move(original) };
    EXPECT_EQ(moved.value, 42);
}

TEST(NonCopyableTest, MoveAssignsDerivedValue) {
    NonCopyableValue original{ .value = 42 };
    NonCopyableValue moved{ .value = 0 };
    moved = std::move(original);
    EXPECT_EQ(moved.value, 42);
}
