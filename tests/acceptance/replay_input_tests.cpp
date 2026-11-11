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

TEST(ReplayInput, Speed200TurningRouteTurnsTwiceThenReleasesMovement)
{
    auto const first = gameBenchmarkSpeed200TurningRouteAt(std::chrono::seconds{ 0 });
    auto const second = gameBenchmarkSpeed200TurningRouteAt(std::chrono::seconds{ 2 });
    auto const third = gameBenchmarkSpeed200TurningRouteAt(std::chrono::seconds{ 4 });
    EXPECT_EQ(first.forward, 1);
    EXPECT_EQ(first.yaw_degrees, 90.0);
    EXPECT_EQ(second.forward, 1);
    EXPECT_EQ(second.yaw_degrees, 0.0);
    EXPECT_EQ(third.forward, 1);
    EXPECT_EQ(third.yaw_degrees, -90.0);
    for (int const seconds : { 6, 7, 60 }) {
        auto const stopped = gameBenchmarkDirection(
            gameBenchmarkSpeed200TurningRouteAt(std::chrono::seconds{ seconds })
        );
        EXPECT_EQ(stopped.x, 0U);
        EXPECT_EQ(stopped.y, 0U);
        EXPECT_EQ(stopped.z, 0U);
        EXPECT_FALSE(stopped.cycle_movement_capabilities);
    }
    auto const first_wire = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200TurningMovement, 0U);
    auto const second_wire = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200TurningMovement, 20U);
    auto const third_wire = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200TurningMovement, 40U);
    EXPECT_EQ(static_cast<int8_t>(first_wire.x), 127);
    EXPECT_EQ(first_wire.y, 0U);
    EXPECT_EQ(second_wire.x, 0U);
    EXPECT_EQ(static_cast<int8_t>(second_wire.y), 127);
    EXPECT_EQ(static_cast<int8_t>(third_wire.x), -127);
    EXPECT_EQ(third_wire.y, 0U);
    for (uint64_t const tick : { 0U, 20U, 40U, 60U, 600U }) {
        auto const wire = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200TurningMovement, tick);
        EXPECT_EQ(wire.speedup, 200U);
        EXPECT_TRUE(wire.accelerated);
        EXPECT_FALSE(wire.cycle_movement_capabilities);
        EXPECT_EQ(wire.z, 0U);
        if (tick >= 60U) {
            EXPECT_EQ(wire.x, 0U);
            EXPECT_EQ(wire.y, 0U);
        }
    }
    EXPECT_EQ(gameBenchmarkWorkloadName(GameBenchmarkWorkload::Speed200TurningMovement),
        "speed-200-turning-movement-v1");
}

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
    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 2'144.0, std::nullopt).vertical, 0);
}

TEST(ReplayInput, ClearanceControlRespectsFlightBoundsAndMissingPlayerPosition)
{
    GameBenchmarkMovementRouteSample const route{};

    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, 1023.0, 400U).vertical, -1);
    EXPECT_EQ(gameBenchmarkApplyFlightClearance(route, std::nullopt, 400U).vertical, 0);
}

TEST(ReplayInput, ClearanceControlClearsUpperTerrainWithinTheSeparateEntityAirspace)
{
    EXPECT_EQ(gameBenchmarkFlightClearanceIntent(1'541.0, 1'540U), 1);
    EXPECT_EQ(gameBenchmarkFlightClearanceIntent(2'048.0, 2'047U), 1);
    EXPECT_EQ(gameBenchmarkFlightClearanceIntent(2'144.0, 2'047U), 0);
    EXPECT_EQ(gameBenchmarkFlightClearanceIntent(2'160.0, 2'047U), -1);
    EXPECT_EQ(gameBenchmarkFlightClearanceIntent(2'048.0, std::nullopt), 1);
    EXPECT_EQ(gameBenchmarkFlightClearanceIntent(2'144.0, std::nullopt), 0);
    EXPECT_EQ(shared::World::FLIGHT_ENTITY_MAX_Z, 65'535);
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
