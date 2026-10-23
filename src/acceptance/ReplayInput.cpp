#include <acceptance/ReplayInput.hpp>

#include <client/CameraController.hpp>
#include <client/PreviewResidency.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <algorithm>
#include <cmath>
#include <compare>
#include <ratio>

namespace acceptance {

namespace {

constexpr std::chrono::seconds DESCENT_START{ 23 };
constexpr uint64_t ROUTE_TICKS = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION).count()
    / std::chrono::duration_cast<std::chrono::nanoseconds>(shared::TICK).count()
);
constexpr uint64_t DESCENT_TICK = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(DESCENT_START).count()
    / std::chrono::duration_cast<std::chrono::nanoseconds>(shared::TICK).count()
);
constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;
constexpr double FLIGHT_CLEARANCE_HOLD_HALF_BAND_BLOCKS =
    GAME_BENCHMARK_FLIGHT_CLEARANCE_HOLD_BAND_BLOCKS / 2.0;
constexpr double FLIGHT_CLEARANCE_SAFE_CEILING =
    static_cast<double>(shared::World::FLIGHT_MAX_Z)
    - GAME_BENCHMARK_UNKNOWN_TERRAIN_TARGET_BELOW_CEILING_BLOCKS;

double interpolate(
    double const start,
    double const end,
    std::chrono::nanoseconds const elapsed,
    std::chrono::nanoseconds const duration
) noexcept
{
    double const fraction = static_cast<double>(elapsed.count()) / static_cast<double>(duration.count());
    return start + (end - start) * std::clamp(fraction, 0.0, 1.0);
}

} // namespace

int8_t gameBenchmarkFlightClearanceIntent(
    std::optional<double> const player_z,
    std::optional<uint16_t> const highest_solid_z
) noexcept
{
    if (!player_z || !std::isfinite(*player_z)) {
        return 0;
    }

    double const target_z = highest_solid_z
        ? std::min(
            static_cast<double>(*highest_solid_z)
                + 1.0
                + GAME_BENCHMARK_FLIGHT_CLEARANCE_TARGET_BLOCKS,
            FLIGHT_CLEARANCE_SAFE_CEILING
        )
        : FLIGHT_CLEARANCE_SAFE_CEILING;
    if (*player_z < target_z - FLIGHT_CLEARANCE_HOLD_HALF_BAND_BLOCKS
        && *player_z < static_cast<double>(shared::World::FLIGHT_MAX_Z)
            - FLIGHT_CLEARANCE_HOLD_HALF_BAND_BLOCKS) {
        return 1;
    }
    if (*player_z > target_z + FLIGHT_CLEARANCE_HOLD_HALF_BAND_BLOCKS && *player_z > 0.0) {
        return -1;
    }
    return 0;
}

GameBenchmarkMovementRouteSample gameBenchmarkApplyFlightClearance(
    GameBenchmarkMovementRouteSample route,
    std::optional<double> const player_z,
    std::optional<uint16_t> const highest_solid_z
) noexcept
{
    route.vertical = gameBenchmarkFlightClearanceIntent(player_z, highest_solid_z);
    return route;
}

std::optional<uint16_t> gameBenchmarkDeliveredSurfaceHeight(
    client::PreviewResidency const& residency,
    shared::TerrainIdentity const* const expected_identity,
    double const world_x,
    double const world_y
) noexcept
{
    if (expected_identity == nullptr || !std::isfinite(world_x) || !std::isfinite(world_y)) {
        return std::nullopt;
    }

    auto const wrapped_block = [](double const coordinate) -> std::optional<uint32_t> {
        double wrapped = std::fmod(
            std::floor(coordinate),
            static_cast<double>(shared::WorldExtent::WIDTH)
        );
        if (!std::isfinite(wrapped)) {
            return std::nullopt;
        }
        if (wrapped < 0.0) {
            wrapped += static_cast<double>(shared::WorldExtent::WIDTH);
        }
        return static_cast<uint32_t>(wrapped);
    };
    std::optional<uint32_t> const block_x = wrapped_block(world_x);
    std::optional<uint32_t> const block_y = wrapped_block(world_y);
    if (!block_x || !block_y) {
        return std::nullopt;
    }

    constexpr uint32_t TILE_SIDE = shared::HEIGHT_TILE_SIDE_LENGTH;
    shared::HeightTileKey const key{
        .x = static_cast<int32_t>(*block_x / TILE_SIDE),
        .y = static_cast<int32_t>(*block_y / TILE_SIDE),
    };
    client::HeightTileHandle const tile = residency.resident(key);
    if (!tile || tile->surface().identity != *expected_identity) {
        return std::nullopt;
    }

    shared::TerrainColumn const* const column = tile->surface().columnAt({
        .x = static_cast<uint8_t>(*block_x % TILE_SIDE),
        .y = static_cast<uint8_t>(*block_y % TILE_SIDE),
        .z = 0U,
    });
    if (!column || !shared::isValidTerrainColumn(
        *column,
        tile->surface().block_sea_level,
        tile->surface().max_height
    )) {
        return std::nullopt;
    }
    return column->highest_solid_z;
}

GameBenchmarkMovementRouteSample gameBenchmarkMovementRouteAt(std::chrono::nanoseconds const elapsed) noexcept
{
    static constexpr std::chrono::seconds FIRST_LOOK_TURN{ 6 };
    static constexpr std::chrono::seconds SECOND_LOOK_TURN{ 16 };
    static constexpr std::chrono::seconds THIRD_LOOK_TURN{ DESCENT_START };
    std::chrono::nanoseconds const bounded_elapsed = std::clamp(
        elapsed,
        std::chrono::nanoseconds::zero(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION)
    );
    if (bounded_elapsed < FIRST_LOOK_TURN) {
        return {
            .forward = 1,
            .vertical = 1,
        };
    }
    if (bounded_elapsed < SECOND_LOOK_TURN) {
        std::chrono::nanoseconds const segment_elapsed = bounded_elapsed - FIRST_LOOK_TURN;
        return {
            .forward = 1,
            .yaw_degrees = interpolate(
                0.0,
                -90.0,
                segment_elapsed,
                std::chrono::duration_cast<std::chrono::nanoseconds>(SECOND_LOOK_TURN - FIRST_LOOK_TURN)
            ),
            .pitch_degrees = interpolate(0.0, -10.0, segment_elapsed,
                std::chrono::duration_cast<std::chrono::nanoseconds>(SECOND_LOOK_TURN - FIRST_LOOK_TURN)),
        };
    }
    if (bounded_elapsed < THIRD_LOOK_TURN) {
        std::chrono::nanoseconds const segment_elapsed = bounded_elapsed - SECOND_LOOK_TURN;
        return {
            .strafe = 1,
            .yaw_degrees = interpolate(
                -90.0,
                -180.0,
                segment_elapsed,
                std::chrono::duration_cast<std::chrono::nanoseconds>(THIRD_LOOK_TURN - SECOND_LOOK_TURN)
            ),
            .pitch_degrees = interpolate(
                -10.0,
                8.0,
                segment_elapsed,
                std::chrono::duration_cast<std::chrono::nanoseconds>(THIRD_LOOK_TURN - SECOND_LOOK_TURN)
            ),
        };
    }
    std::chrono::nanoseconds const segment_elapsed = bounded_elapsed - THIRD_LOOK_TURN;
    return {
        .forward = -1,
        .vertical = -1,
        .yaw_degrees = interpolate(
            -180.0,
            0.0,
            segment_elapsed,
            std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION - THIRD_LOOK_TURN)
        ),
        .pitch_degrees = interpolate(
            8.0,
            0.0,
            segment_elapsed,
            std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION - THIRD_LOOK_TURN)
        ),
    };
}

GameBenchmarkMovementRouteSample gameBenchmarkRepeatingMovementRouteAt(
    std::chrono::nanoseconds const elapsed
) noexcept
{
    std::chrono::nanoseconds const route_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION
    );
    std::chrono::nanoseconds const nonnegative_elapsed = std::max(elapsed, std::chrono::nanoseconds::zero());
    return gameBenchmarkMovementRouteAt(nonnegative_elapsed % route_duration);
}

uint64_t gameBenchmarkMovementRouteCycleAt(std::chrono::nanoseconds const elapsed) noexcept
{
    if (elapsed <= std::chrono::nanoseconds::zero()) {
        return 0U;
    }
    return static_cast<uint64_t>(
        elapsed.count()
        / std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_MOVEMENT_ROUTE_DURATION).count()
    );
}

shared::Direction gameBenchmarkDirection(GameBenchmarkMovementRouteSample const& route) noexcept
{
    client::MovementDirection const movement = client::CameraController::cameraRelativeMovement(
        {
            .strafe = route.strafe,
            .forward = route.forward,
        },
        route.yaw_degrees
    );
    double const yaw_radians = route.yaw_degrees * DEGREES_TO_RADIANS;
    double const pitch_radians = route.pitch_degrees * DEGREES_TO_RADIANS;
    double const pitch_scale = std::cos(pitch_radians);
    return shared::Direction{
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
        .z = static_cast<uint8_t>(client::CameraController::verticalMovement(
            route.vertical > 0,
            route.vertical < 0
        )),
        .accelerated = true,
        .speedup = 30U,
        .cycle_movement_capabilities = false,
        .view_x = client::CameraController::quantize(std::sin(yaw_radians) * pitch_scale),
        .view_y = client::CameraController::quantize(std::cos(yaw_radians) * pitch_scale),
    };
}

bool gameBenchmarkStartActionComplete(GameBenchmarkStartActionEvidence const& evidence) noexcept
{
    if (!evidence.complete
        || evidence.input_source != "synthetic-controlled-start-key"
        || evidence.production_path
            != "GameClientBenchmarkHooks.start_input_override->GameClient.acceptLoadingStartInput"
        || !evidence.synthetic_input_override_invoked
        || !evidence.input_ordinal
        || !evidence.eligible_prompt_rendered_offset_ns
        || !evidence.synthetic_input_requested_offset_ns
        || !evidence.key_accepted_offset_ns
        || !evidence.server_start_acknowledged_offset_ns) {
        return false;
    }
    return *evidence.eligible_prompt_rendered_offset_ns
            <= *evidence.synthetic_input_requested_offset_ns
        && *evidence.synthetic_input_requested_offset_ns <= *evidence.key_accepted_offset_ns
        && *evidence.key_accepted_offset_ns <= *evidence.server_start_acknowledged_offset_ns;
}

shared::Direction gameBenchmarkDirection(
    GameBenchmarkWorkload const workload,
    uint64_t const ordinal
) noexcept
{
    if (workload == GameBenchmarkWorkload::DiagnosticStationary) {
        return {};
    }
    if (workload == GameBenchmarkWorkload::OrdinaryTurningMovement) {
        uint64_t const route_tick = std::min(ordinal, ROUTE_TICKS);
        GameBenchmarkMovementRouteSample route = gameBenchmarkMovementRouteAt(
            std::chrono::duration_cast<std::chrono::nanoseconds>(shared::TICK)
                * static_cast<int64_t>(route_tick)
        );
        if (route_tick != 0U && route_tick != DESCENT_TICK) {
            route.vertical = 0;
        }
        return gameBenchmarkDirection(route);
    }
    bool const accelerated_full_axis = workload == GameBenchmarkWorkload::Speed200Movement
        || workload == GameBenchmarkWorkload::WrappedBorder
        || workload == GameBenchmarkWorkload::Speed200FullRadiusRecovery;
    int8_t const x = accelerated_full_axis ? int8_t{127}
        : (ordinal % 40U < 20U ? int8_t{127} : int8_t{-127});
    uint16_t const speedup = accelerated_full_axis ? 200U : 5U;
    return shared::Direction{
        .x = static_cast<uint8_t>(x),
        .y = 0U,
        .accelerated = accelerated_full_axis,
        .speedup = speedup,
        .cycle_movement_capabilities = false,
    };
}

std::string_view gameBenchmarkWorkloadName(GameBenchmarkWorkload const workload) noexcept
{
    switch (workload) {
        case GameBenchmarkWorkload::OrdinaryMovement: return "ordinary-movement-v2";
        case GameBenchmarkWorkload::OrdinaryTurningMovement: return "turning-x30-movement-v3";
        case GameBenchmarkWorkload::Speed200Movement: return "speed-200-movement-v2";
        case GameBenchmarkWorkload::WrappedBorder: return "wrapped-border-v1";
        case GameBenchmarkWorkload::PermissionCollisionChurn: return "permission-collision-churn-v2";
        case GameBenchmarkWorkload::DiagnosticStationary: return "diagnostic-stationary-v1";
        case GameBenchmarkWorkload::Speed200FullRadiusRecovery: return "speed-200-full-radius-recovery-v1";
    }
    return "unknown";
}

} // namespace acceptance
