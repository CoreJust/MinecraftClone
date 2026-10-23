#include <client/PreviewResidency.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <acceptance/ReplayInput.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace acceptance {

namespace {

TEST(ReplayInput, ClearanceControlClimbsBelowTheKnownTerrainTarget)
{
    GameBenchmarkMovementRouteSample const route{
        .strafe = 1,
        .forward = 1,
        .vertical = 0,
        .yaw_degrees = -45.0,
        .pitch_degrees = 8.0,
    };

    GameBenchmarkMovementRouteSample const controlled = gameBenchmarkApplyFlightClearance(route, 480.0, 400U);
    shared::Direction const before = gameBenchmarkDirection(route);
    shared::Direction const after = gameBenchmarkDirection(controlled);

    EXPECT_EQ(controlled.vertical, 1);
    EXPECT_EQ(controlled.strafe, route.strafe);
    EXPECT_EQ(controlled.forward, route.forward);
    EXPECT_EQ(controlled.yaw_degrees, route.yaw_degrees);
    EXPECT_EQ(controlled.pitch_degrees, route.pitch_degrees);
    EXPECT_EQ(after.x, before.x);
    EXPECT_EQ(after.y, before.y);
    EXPECT_EQ(after.view_x, before.view_x);
    EXPECT_EQ(after.view_y, before.view_y);
    EXPECT_EQ(after.speedup, before.speedup);
    EXPECT_EQ(static_cast<int8_t>(after.z), 127);
}

TEST(ReplayInput, ClearanceControlDescendsAboveAndHoldsInsideTheTargetBand)
{
    GameBenchmarkMovementRouteSample const route{ .forward = 1, .vertical = 1 };

    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 520.0, 400U).vertical, -1);
    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 497.0, 400U).vertical, 0);
    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 508.0, 400U).vertical, 0);
}

TEST(ReplayInput, UnknownTerrainClimbsToTheBoundedFallbackWithoutInventingHeight)
{
    GameBenchmarkMovementRouteSample const route{ .forward = 1, .vertical = 0 };

    GameBenchmarkMovementRouteSample const controlled = gameBenchmarkApplyFlightClearance(
        route,
        950.0,
        std::nullopt
    );

    EXPECT_EQ(controlled.vertical, 1);
    EXPECT_EQ(controlled.strafe, route.strafe);
    EXPECT_EQ(controlled.forward, route.forward);
    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 991.0, std::nullopt).vertical, 0);
}

TEST(ReplayInput, ClearanceControlRespectsFlightBoundsAndMissingPlayerPosition)
{
    GameBenchmarkMovementRouteSample const route{};

    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 1023.0, 400U).vertical, -1);
    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, std::nullopt, 400U).vertical, 0);
}

TEST(ReplayInput, MissingDeliveredTerrainDoesNotProduceAHeight)
{
    client::PreviewResidency const residency{ client::HeightTileRevision{ .generation = 1U, .revision = 1U } };
    shared::TerrainIdentity const identity{ .seed = 42U, .configuration_identity = "terrain-v5" };

    EXPECT_FALSE(gameBenchmarkDeliveredSurfaceHeight(residency, &identity, 32.0, 48.0));
    EXPECT_FALSE(gameBenchmarkDeliveredSurfaceHeight(residency, nullptr, 32.0, 48.0));
}

} // namespace

} // namespace acceptance
