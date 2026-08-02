#include <core/algorithm/TopoSort.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

TEST(TopoSortTest, OrdersDependenciesBeforeDependents) {
    std::array<std::vector<size_t>, 4> successors{
        std::vector<size_t>{ 1, 2 },
        std::vector<size_t>{ 3 },
        std::vector<size_t>{ 3 },
        std::vector<size_t>{ },
    };

    auto const sorted = core::topoSort(successors.size(), [&](size_t const node) {
        return successors[node];
    });

    ASSERT_TRUE(sorted.has_value());
    auto const position = [&](size_t const node) {
        return std::find(sorted->begin(), sorted->end(), node) - sorted->begin();
    };
    EXPECT_LT(position(0), position(1));
    EXPECT_LT(position(0), position(2));
    EXPECT_LT(position(1), position(3));
    EXPECT_LT(position(2), position(3));
}

TEST(TopoSortTest, DetectsCycles) {
    std::array<std::vector<size_t>, 2> successors{
        std::vector<size_t>{ 1 },
        std::vector<size_t>{ 0 },
    };

    EXPECT_FALSE(core::topoSort(successors.size(), [&](size_t const node) {
        return successors[node];
    }));
}

TEST(TopoSortTest, HandlesDisconnectedNodes) {
    std::array<std::vector<size_t>, 3> successors{
        std::vector<size_t>{ },
        std::vector<size_t>{ },
        std::vector<size_t>{ },
    };

    auto const sorted = core::topoSort(successors.size(), [&](size_t const node) {
        return successors[node];
    });

    ASSERT_TRUE(sorted.has_value());
    EXPECT_EQ(sorted->size(), 3u);
}
