#include <client/TerrainLodRuntime.hpp>

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace client {
namespace {

TEST(TerrainLodRuntimeTest, CrossingCenterRetainsPresentedCurrentColumns)
{
    static constexpr std::array<TerrainLodColumnKey, 3> COLUMNS{{{3U, 2U}, {4U, 2U}, {5U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    for (auto const column : COLUMNS) {
        plan.remember(column);
    }
    plan.presented(COLUMNS);
    plan.updateInterest(64U, 32U);
    EXPECT_EQ(plan.ticket().columns.size(), COLUMNS.size());
    EXPECT_FALSE(plan.canPublish(plan.ticket(), std::span{COLUMNS}.subspan(1U, 1U)));
}

TEST(TerrainLodRuntimeTest, DelayedSceneCannotOmitCurrentReadyCenter)
{
    static constexpr TerrainLodColumnKey OLD{3U, 2U};
    static constexpr TerrainLodColumnKey CURRENT{8U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    plan.remember(OLD);
    auto const old = plan.ticket();
    plan.updateInterest(128U, 32U);
    plan.remember(CURRENT);
    EXPECT_FALSE(plan.canPublish(old, std::array{OLD}));
}

TEST(TerrainLodRuntimeTest, DiscoveryAdvancesAcrossRepeatedCenterChanges)
{
    static constexpr std::array<TerrainLodColumnKey, 3> AVAILABLE{{{10U, 2U}, {11U, 2U}, {12U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[0]);
    plan.remember(AVAILABLE[0]);
    plan.updateInterest(128U, 32U);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[1]);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[2]);
}

TEST(TerrainLodRuntimeTest, ForgettingInvalidSourceAllowsItsReplacementToBeOmitted)
{
    static constexpr std::array<TerrainLodColumnKey, 2> COLUMNS{{{3U, 2U}, {4U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    for (auto const key : COLUMNS) {
        plan.remember(key);
    }
    plan.presented(COLUMNS);
    plan.forget(COLUMNS[0]);
    auto const ticket = plan.ticket();
    EXPECT_TRUE(plan.canPublish(ticket, std::span{COLUMNS}.subspan(1U)));
}

TEST(TerrainLodRuntimeTest, OlderInterestMayPublishWhenItRetainsAllCurrentValidDetail)
{
    static constexpr std::array<TerrainLodColumnKey, 2> COLUMNS{{{3U, 2U}, {4U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    for (auto const key : COLUMNS) {
        plan.remember(key);
    }
    plan.presented(COLUMNS);
    auto const ticket = plan.ticket();
    plan.updateInterest(64U, 32U);
    EXPECT_TRUE(plan.canPublish(ticket, COLUMNS));
}

TEST(TerrainLodRuntimeTest, DistantColumnsRetireOnlyAfterLeavingPreservationInterest)
{
    static constexpr TerrainLodColumnKey OLD{0U, 0U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    plan.remember(OLD);
    plan.presented(std::array{OLD});
    plan.updateInterest(640U, 0U);
    EXPECT_EQ(plan.ticket().columns.size(), 1U);
    plan.updateInterest(672U, 0U);
    EXPECT_TRUE(plan.ticket().columns.empty());
    EXPECT_TRUE(plan.canPublish(plan.ticket(), {}));
}

TEST(TerrainLodRuntimeTest, ReadyCurrentCenterPreemptsPreviouslyQueuedDistantColumns)
{
    static constexpr std::array<TerrainLodColumnKey, 3> AVAILABLE{{{10U, 0U}, {11U, 0U}, {12U, 0U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    plan.discover(AVAILABLE);
    plan.updateInterest(192U, 0U);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[2]);
}

TEST(TerrainLodRuntimeTest, SourceInvalidationCanRediscoverAnAlreadyPresentedColumn)
{
    static constexpr std::array<TerrainLodColumnKey, 1> AVAILABLE{{{0U, 0U}}};
    TerrainLodRuntimePlan plan;
    plan.remember(AVAILABLE[0]);
    plan.discover(AVAILABLE);
    EXPECT_FALSE(plan.nextDiscovery());
    plan.forget(AVAILABLE[0]);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[0]);
}

TEST(TerrainLodRuntimeTest, RelevantColumnsIncludeFortyChunksAndPeriodicWrap)
{
    TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    EXPECT_TRUE(plan.relevant({40U, 0U}));
    EXPECT_TRUE(plan.relevant({4'056U, 0U}));
    EXPECT_FALSE(plan.relevant({42U, 0U}));
}

TEST(TerrainLodRuntimeTest, SourceResetRejectsPriorWork)
{
    TerrainLodRuntimePlan plan;
    auto const old = plan.ticket();
    plan.reset();
    EXPECT_FALSE(plan.canPublish(old, {}));
}

TEST(TerrainLodRuntimeTest, PublicationReportsMissingCurrentCenter)
{
    static constexpr TerrainLodColumnKey OLD{3U, 2U};
    static constexpr TerrainLodColumnKey CURRENT{8U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    plan.remember(OLD);
    auto const ticket = plan.ticket();
    plan.updateInterest(128U, 32U);
    plan.remember(CURRENT);
    auto const issue = plan.publicationIssue(ticket, std::array{OLD});
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::MissingCenter);
    EXPECT_EQ(issue.column, CURRENT);
}

TEST(TerrainLodRuntimeTest, PublicationReportsRetainedPresentedDetail)
{
    static constexpr TerrainLodColumnKey CENTER{4U, 2U};
    static constexpr TerrainLodColumnKey RETAINED{5U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    plan.remember(CENTER);
    plan.remember(RETAINED);
    plan.presented(std::array{CENTER, RETAINED});
    auto const issue = plan.publicationIssue(plan.ticket(), std::array{CENTER});
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::MissingPresented);
    EXPECT_EQ(issue.column, RETAINED);
}

TEST(TerrainLodRuntimeTest, PublicationReportsUncapturedColumn)
{
    static constexpr TerrainLodColumnKey CAPTURED{4U, 2U};
    static constexpr TerrainLodColumnKey UNCAPTURED{5U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    plan.remember(CAPTURED);
    auto const issue = plan.publicationIssue(plan.ticket(), std::array{CAPTURED, UNCAPTURED});
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::UncapturedColumn);
    EXPECT_EQ(issue.column, UNCAPTURED);
}

} // namespace
} // namespace client
