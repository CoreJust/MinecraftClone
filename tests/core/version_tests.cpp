#include <core/common/Version.hpp>

#include <gtest/gtest.h>

#include <fmt/format.h>

TEST(VersionTest, ComparesComponentsInOrder) {
    core::Version const base{ .epoch = 1, .major = 2, .minor = 3, .patch = 4 };

    EXPECT_LT((core::Version{ 0, 9, 9, 9 }), base);
    EXPECT_LT((core::Version{ 1, 1, 9, 9 }), base);
    EXPECT_LT((core::Version{ 1, 2, 2, 9 }), base);
    EXPECT_LT((core::Version{ 1, 2, 3, 3 }), base);
    EXPECT_FALSE(base < (core::Version{ 1, 2, 3, 4 }));
    EXPECT_FALSE((core::Version{ 1, 2, 3, 4 }) < base);
}

TEST(VersionTest, MaxIsGreaterThanNormalVersions) {
    EXPECT_GT(core::Version::MAX(), (core::Version{ 0, 1, 0, 0 }));
}

TEST(VersionTest, FormatsAsEpochMajorMinorPatch) {
    EXPECT_EQ(fmt::format("{}", core::Version{ 1, 2, 3, 4 }), "1.2.3:4");
}
