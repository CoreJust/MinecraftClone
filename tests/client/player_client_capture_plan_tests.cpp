#include <client/Camera.hpp>
#include <client/PlayerClient.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewResidency.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

TEST(PlayerClientCapturePlanTest, FramesTheCentralSpikeAndFirstRingTogether)
{
    static constexpr int32_t WORLD_CENTER = 32'768;
    static constexpr int32_t PREVIOUS_PLAYER_DISTANCE = 243;
    static constexpr int32_t PLAYER_DISTANCE = 170;
    static constexpr int32_t PREVIOUS_PLAYER_HEIGHT = 440;
    static constexpr int32_t PLAYER_HEIGHT = 420;
    static constexpr int32_t FIRST_RING_RADIUS = 128;
    static constexpr uint32_t FRAME_WIDTH = 2560U;
    static constexpr uint32_t FRAME_HEIGHT = 1440U;
    client::PlayerClientCapturePlan const plan = client::playerClientCapturePlan(
        client::PlayerClientCapturePreset::CentralSpike
    );
    EXPECT_EQ(plan.target.x, WORLD_CENTER - PLAYER_DISTANCE);
    EXPECT_EQ(plan.target.y, WORLD_CENTER);
    EXPECT_EQ(plan.target.z, PLAYER_HEIGHT);
    EXPECT_NEAR(
        static_cast<double>(PREVIOUS_PLAYER_DISTANCE - PLAYER_DISTANCE) / PREVIOUS_PLAYER_DISTANCE,
        0.30,
        0.01
    );
    EXPECT_EQ(PREVIOUS_PLAYER_HEIGHT - PLAYER_HEIGHT, 20);
    EXPECT_EQ(plan.look_angles.yaw_degrees, 110.0);
    EXPECT_EQ(plan.look_angles.pitch_degrees, 18.0);
    EXPECT_EQ(plan.vertical_fov_degrees, 105.0);
    EXPECT_EQ(plan.rear_camera_distance, 12.0);

    shared::TerrainGenerator const terrain;
    EXPECT_GT(plan.target.z, terrain.heightAt(plan.target.x, plan.target.y));
    EXPECT_LT(plan.target.z, terrain.heightAt(WORLD_CENTER, WORLD_CENTER));
    int32_t const near_ring_x = WORLD_CENTER - FIRST_RING_RADIUS;
    int32_t const far_ring_x = WORLD_CENTER + FIRST_RING_RADIUS;
    uint16_t const near_ring_height = terrain.heightAt(near_ring_x, WORLD_CENTER);
    uint16_t const far_ring_height = terrain.heightAt(far_ring_x, WORLD_CENTER);
    EXPECT_EQ(near_ring_height, 406U);
    EXPECT_EQ(far_ring_height, 406U);

    client::PlayerPresentationPosition const player_position{
        .x = static_cast<double>(plan.target.x),
        .y = static_cast<double>(plan.target.y),
        .z = static_cast<double>(plan.target.z),
    };
    glm::dvec3 const eye = client::localPlayerEyePosition(player_position);
    client::Camera const look_camera{{ .position = eye, .angles = plan.look_angles }};
    client::Camera const camera{
        { .position = eye - look_camera.forward() * plan.rear_camera_distance, .angles = plan.look_angles },
        { .vertical_fov_degrees = plan.vertical_fov_degrees },
    };
    auto const projection = camera.projectionMatrix(FRAME_WIDTH, FRAME_HEIGHT);
    ASSERT_TRUE(projection.has_value());
    auto const clip_coordinates = [&](glm::vec4 const world_point) {
        return *projection * camera.viewMatrix() * world_point;
    };
    auto const projected = [&](glm::vec4 const world_point) {
        glm::vec4 const clip = clip_coordinates(world_point);
        return glm::vec3(clip) / clip.w;
    };
    glm::vec3 const peak = projected({
        static_cast<float>(WORLD_CENTER), static_cast<float>(WORLD_CENTER),
        static_cast<float>(terrain.heightAt(WORLD_CENTER, WORLD_CENTER)), 1.0F,
    });
    glm::vec3 const near_ring = projected({
        static_cast<float>(near_ring_x), static_cast<float>(WORLD_CENTER),
        static_cast<float>(near_ring_height), 1.0F,
    });
    glm::vec3 const far_ring = projected({
        static_cast<float>(far_ring_x), static_cast<float>(WORLD_CENTER),
        static_cast<float>(far_ring_height), 1.0F,
    });
    static constexpr int32_t BACKGROUND_RADIUS = FIRST_RING_RADIUS * 2;
    glm::vec4 const background_clip = clip_coordinates({
        static_cast<float>(WORLD_CENTER), static_cast<float>(WORLD_CENTER - BACKGROUND_RADIUS),
        static_cast<float>(terrain.heightAt(WORLD_CENTER, WORLD_CENTER - BACKGROUND_RADIUS)), 1.0F,
    });
    glm::vec3 const background = glm::vec3(background_clip) / background_clip.w;
    glm::dvec3 const camera_position = camera.pose().position;
    double const segment_x = static_cast<double>(WORLD_CENTER) - camera_position.x;
    double const segment_y = static_cast<double>(WORLD_CENTER - BACKGROUND_RADIUS) - camera_position.y;
    double const camera_radius_x = camera_position.x - WORLD_CENTER;
    double const camera_radius_y = camera_position.y - WORLD_CENTER;
    double const nearest_segment_parameter = std::clamp(
        -(camera_radius_x * segment_x + camera_radius_y * segment_y)
            / (segment_x * segment_x + segment_y * segment_y),
        0.0,
        1.0
    );
    double const nearest_background_sightline_radius = std::hypot(
        camera_radius_x + nearest_segment_parameter * segment_x,
        camera_radius_y + nearest_segment_parameter * segment_y
    );
    EXPECT_GT(near_ring.y, 0.0F);
    EXPECT_GT(far_ring.y, 0.0F);
    EXPECT_LT(peak.y, 0.0F);
    EXPECT_LT(std::abs(near_ring.x), 0.9F);
    EXPECT_LT(std::abs(near_ring.y), 0.9F);
    EXPECT_LT(std::abs(far_ring.x), 0.9F);
    EXPECT_LT(std::abs(far_ring.y), 0.9F);
    EXPECT_LT(std::abs(peak.x), 0.9F);
    EXPECT_LT(std::abs(peak.y), 0.9F);
    EXPECT_GT(background_clip.w, 0.0F);
    EXPECT_GE(background.z, 0.0F);
    EXPECT_LE(background.z, 1.0F);
    EXPECT_GT(terrain.heightAt(WORLD_CENTER, WORLD_CENTER - BACKGROUND_RADIUS), 0U);
    EXPECT_GT(nearest_background_sightline_radius, FIRST_RING_RADIUS);
    EXPECT_GT(background.x, 0.25F);
    EXPECT_LT(background.x, 0.8F);
    EXPECT_GT(background.y, 0.55F);
    EXPECT_LT(background.y, 0.85F);
}

TEST(PlayerClientCapturePlanTest, FramesTheUpperFirstRingAsAnAscendingSlope)
{
    static constexpr int32_t WORLD_CENTER = 32'768;
    static constexpr int32_t FIRST_RING_ASCENT_RADIUS = 112;
    static constexpr int32_t ASCENT_SAMPLE_DISTANCE = 16;
    static constexpr uint16_t MINIMUM_ASCENT = 50U;
    static constexpr double MINIMUM_CLIMBING_PITCH = 45.0;
    static constexpr double MAXIMUM_CLIMBING_PITCH = 65.0;
    static constexpr double MINIMUM_ASCENT_YAW_OFFSET = 5.0;
    static constexpr double MAXIMUM_ASCENT_YAW_OFFSET = 20.0;
    client::PlayerClientCapturePlan const plan = client::playerClientCapturePlan(
        client::PlayerClientCapturePreset::MountainClimb
    );
    shared::TerrainGenerator const terrain;
    uint16_t const target_height = terrain.heightAt(plan.target.x, plan.target.y);

    EXPECT_EQ(plan.target.x, WORLD_CENTER + FIRST_RING_ASCENT_RADIUS);
    EXPECT_EQ(plan.target.y, WORLD_CENTER);
    EXPECT_EQ(plan.target.z, target_height);
    EXPECT_LT(terrain.heightAt(plan.target.x - ASCENT_SAMPLE_DISTANCE, plan.target.y), target_height);
    EXPECT_GE(
        terrain.heightAt(plan.target.x + ASCENT_SAMPLE_DISTANCE, plan.target.y) - target_height,
        MINIMUM_ASCENT
    );
    double const ascent_yaw_offset = std::abs(plan.look_angles.yaw_degrees - 90.0);
    EXPECT_GE(ascent_yaw_offset, MINIMUM_ASCENT_YAW_OFFSET);
    EXPECT_LE(ascent_yaw_offset, MAXIMUM_ASCENT_YAW_OFFSET);
    EXPECT_GE(plan.look_angles.pitch_degrees, MINIMUM_CLIMBING_PITCH);
    EXPECT_LE(plan.look_angles.pitch_degrees, MAXIMUM_CLIMBING_PITCH);
}

TEST(PlayerClientCapturePlanTest, RequiresMountainCaptureWithBothMovementCapabilitiesOff)
{
    client::PlayerClientCapturePlan const plan = client::playerClientCapturePlan(
        client::PlayerClientCapturePreset::MountainClimb
    );

    ASSERT_TRUE(plan.final_movement_capabilities.has_value());
    EXPECT_EQ(plan.final_movement_capabilities->bits, 0U);
}

TEST(PlayerClientCapturePlanTest, BoundsFullTerrainCaptureReadinessToFifteenMinutes)
{
    client::PlayerClientCaptureOptions const options{};

    EXPECT_EQ(options.readiness_deadline, std::chrono::minutes{15});
    EXPECT_EQ(options.sweep_deadline, std::chrono::minutes{5});
    EXPECT_EQ(options.minimum_height_tile_meshes, 0U);
}

TEST(PlayerClientCapturePlanTest, GivesArenaGrowthTimeThroughoutCapturePreload)
{
    EXPECT_EQ(client::playerClientRendererFrameBudget(true), std::chrono::seconds{1});
    EXPECT_EQ(client::playerClientRendererFrameBudget(false), std::chrono::milliseconds{8});
}

TEST(PlayerClientCapturePlanTest, DrainsFullReceiveWindowDuringCapturePreload)
{
    EXPECT_EQ(client::playerClientHeightTileChangeBudget(true), 128U);
    EXPECT_EQ(client::playerClientHeightTileChangeBudget(false), 128U);
}

TEST(PlayerClientCapturePlanTest, UsesTheSamePreviewMeshJobBudgetDuringCaptureAndPlay)
{
    EXPECT_EQ(client::playerClientPreviewMeshJobBudget(true), 16U);
    EXPECT_EQ(client::playerClientPreviewMeshJobBudget(false), 16U);
}

TEST(PlayerClientCapturePlanTest, DrainsOneBoundedProtocolWindowOfResidencyChangesPerFrame)
{
    static constexpr uint32_t PROTOCOL_WINDOW = 128U;
    static constexpr uint32_t CHANGE_COUNT = PROTOCOL_WINDOW + 1U;
    static constexpr client::HeightTileRevision REVISION{ .generation = 1U, .revision = 1U };
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights{};
    client::PreviewResidency residency{ REVISION };

    for (uint32_t index = 0U; index < CHANGE_COUNT; ++index) {
        shared::HeightTileKey const key{ static_cast<int32_t>(index), 1 };
        ASSERT_EQ(
            residency.accept(key, REVISION, static_cast<uint64_t>(index) + 1U, heights).replacement,
            client::HeightTileReplacement::Published
        );
    }

    std::vector<client::HeightTileChange> const first_window = residency.takeChanges(
        client::playerClientHeightTileChangeBudget(false)
    );
    ASSERT_EQ(first_window.size(), PROTOCOL_WINDOW);
    EXPECT_EQ(residency.pendingChangeCount(), 1U);
    EXPECT_EQ(first_window.front().key.x, 0);
    EXPECT_EQ(first_window.back().key.x, static_cast<int32_t>(PROTOCOL_WINDOW - 1U));

    std::vector<client::HeightTileChange> const second_window = residency.takeChanges(
        client::playerClientHeightTileChangeBudget(false)
    );
    ASSERT_EQ(second_window.size(), 1U);
    EXPECT_EQ(second_window.front().key.x, static_cast<int32_t>(PROTOCOL_WINDOW));
    EXPECT_EQ(residency.pendingChangeCount(), 0U);
}

TEST(PlayerClientCapturePlanTest, WorkerAdmissionWindowBoundsOutstandingJobsAndReopensOnConsumption)
{
    static constexpr uint32_t PLAY_CAPACITY = 128U;
    static constexpr uint32_t CAPTURE_CAPACITY = 16U;
    EXPECT_EQ(client::playerClientPreviewMeshAdmissionBudget(false), PLAY_CAPACITY);
    EXPECT_EQ(client::playerClientPreviewMeshAdmissionBudget(true), CAPTURE_CAPACITY);
    client::PlayerPreviewMeshAdmissionWindow play_window{
        client::playerClientPreviewMeshBacklogCapacity(false)
    };
    client::PlayerPreviewMeshAdmissionWindow capture_window{
        client::playerClientPreviewMeshBacklogCapacity(true)
    };

    for (uint32_t index = 0U; index < PLAY_CAPACITY; ++index) {
        ASSERT_TRUE(play_window.tryAdmit()) << index;
    }
    EXPECT_FALSE(play_window.canAccept());
    EXPECT_FALSE(play_window.tryAdmit());
    ASSERT_TRUE(play_window.releaseOne());
    ASSERT_TRUE(play_window.tryAdmit());
    EXPECT_EQ(play_window.outstandingCount(), PLAY_CAPACITY);
    for (uint32_t index = 0U; index < PLAY_CAPACITY; ++index) {
        ASSERT_TRUE(play_window.releaseOne()) << index;
    }
    EXPECT_FALSE(play_window.releaseOne());
    EXPECT_EQ(play_window.outstandingCount(), 0U);

    for (uint32_t index = 0U; index < CAPTURE_CAPACITY; ++index) {
        ASSERT_TRUE(capture_window.tryAdmit()) << index;
    }
    EXPECT_FALSE(capture_window.canAccept());
    EXPECT_FALSE(capture_window.tryAdmit());
    EXPECT_EQ(capture_window.outstandingCount(), CAPTURE_CAPACITY);
}

TEST(PlayerClientCapturePlanTest, DepartedPreviewRemovalsProgressBeforeCoverageAndKeepRestoredKeys)
{
    EXPECT_FALSE(client::playerClientShouldQueuePreviewRemoval(true, false));
    EXPECT_TRUE(client::playerClientShouldQueuePreviewRemoval(true, true));
    EXPECT_EQ(client::playerClientPreviewRemovalBudget(), 128U);
    EXPECT_TRUE(client::playerClientShouldQueuePreviewRemoval(false, false));

    std::deque<uint32_t> pending{ 10U, 20U, 30U };
    std::unordered_set<uint32_t> current_interest{ 20U };
    std::vector<uint32_t> removed;
    uint32_t const budget = client::playerClientPreviewRemovalBudget();
    for (uint32_t processed = 0U; processed < budget && !pending.empty(); ++processed) {
        uint32_t const key = pending.front();
        pending.pop_front();
        if (client::playerClientShouldRemovePreviewMesh(current_interest.contains(key))) {
            removed.push_back(key);
        }
    }

    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(removed, (std::vector<uint32_t>{ 10U, 30U }));
    EXPECT_FALSE(client::playerClientShouldRemovePreviewMesh(current_interest.contains(20U)));
}

TEST(PlayerClientCapturePlanTest, RequestsPhysicalHighResolutionAtOneAndTwoTimesScale)
{
    EXPECT_EQ(client::playerClientCaptureLogicalSize(1280U, 1280U, 2560U), 2560U);
    EXPECT_EQ(client::playerClientCaptureLogicalSize(720U, 720U, 1440U), 1440U);
    EXPECT_EQ(client::playerClientCaptureLogicalSize(1280U, 2560U, 2560U), 1280U);
    EXPECT_EQ(client::playerClientCaptureLogicalSize(720U, 1440U, 1440U), 720U);
}

TEST(PlayerClientCapturePlanTest, EnforcesThePortableCaptureExtentFloor)
{
    static constexpr uint32_t MINIMUM_WIDTH = 1'000U;
    static constexpr uint32_t MINIMUM_HEIGHT = 600U;
    EXPECT_TRUE(client::playerClientCaptureExtentMeetsMinimum(MINIMUM_WIDTH, MINIMUM_HEIGHT));
    EXPECT_TRUE(client::playerClientCaptureExtentMeetsMinimum(2560U, 1440U));
    EXPECT_FALSE(client::playerClientCaptureExtentMeetsMinimum(MINIMUM_WIDTH - 1U, MINIMUM_HEIGHT));
    EXPECT_FALSE(client::playerClientCaptureExtentMeetsMinimum(MINIMUM_WIDTH, MINIMUM_HEIGHT - 1U));
}

TEST(PlayerClientCapturePlanTest, NearTargetNavigationUsesAProtocolValidSpeedup)
{
    static constexpr int64_t NEAR_DISTANCE_SUBCELLS = 40'000;
    uint16_t const speedup = client::playerClientCaptureSpeedup(NEAR_DISTANCE_SUBCELLS);
    ASSERT_TRUE(shared::isFlightSpeedupProfile(speedup));
    EXPECT_EQ(speedup, 2U);

    shared::ClientInputMessage const input{
        .direction = { .x = 1U, .y = 0U, .accelerated = false, .speedup = speedup },
        .sequence = 1U,
    };
    auto const decoded = shared::decodeMessage(shared::encodeMessage(input));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ClientInputMessage>(*decoded));
    EXPECT_EQ(std::get<shared::ClientInputMessage>(*decoded).direction.speedup, speedup);
}

TEST(PlayerClientCapturePlanTest, StaleVisibleMeshCannotSatisfyResidentCoverage)
{
    static constexpr uint32_t EXPECTED_TILES = 1U;
    static constexpr shared::HeightTileKey KEY{ .x = 1, .y = 1 };
    static constexpr client::HeightTileRevision REVISION{ .generation = 1U, .revision = 1U };
    client::PreviewResidency residency{ REVISION };
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{ KEY };
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const visible_meshes{ KEY };
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights{};

    ASSERT_EQ(
        residency.accept(KEY, REVISION, 1U, heights).replacement,
        client::HeightTileReplacement::Published
    );
    EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 0U
    ));
    static_cast<void>(residency.takeChanges(1U));
    EXPECT_TRUE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 0U
    ));
    EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 1U
    ));

    ASSERT_TRUE(residency.evict(KEY, REVISION, 2U));
    static_cast<void>(residency.takeChanges(1U));
    EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 0U
    ));
}

TEST(PlayerClientCapturePlanTest, PendingMeshesPopByStablePriorityWithoutDuplicates)
{
    shared::HeightTileKey const center{ .x = 100, .y = 100 };
    std::vector<shared::HeightTileKey> keys{
        {.x = 120, .y = 100}, {.x = 100, .y = 120}, {.x = 80, .y = 100},
        {.x = 100, .y = 80}, {.x = 101, .y = 100},
    };
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        keys.begin(), keys.end()
    };
    client::PlayerPreviewMeshQueue queue;
    queue.resetPriority(center, 127, 0, interest);
    for (shared::HeightTileKey const key : keys) {
        EXPECT_TRUE(queue.push(key));
        EXPECT_FALSE(queue.push(key));
    }
    EXPECT_EQ(queue.size(), keys.size());
    std::ranges::sort(keys, [center](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return std::tuple{ shared::heightTileInterestPriority(center, 127, 0, first), first.y, first.x }
            < std::tuple{ shared::heightTileInterestPriority(center, 127, 0, second), second.y, second.x };
    });
    for (shared::HeightTileKey const key : keys) {
        EXPECT_EQ(queue.top(), key);
        queue.pop();
    }
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest, PendingMeshesReprioritizePruneAndRetainFailedEnqueueTop)
{
    shared::HeightTileKey const east{ .x = 140, .y = 100 };
    shared::HeightTileKey const west{ .x = 60, .y = 100 };
    shared::HeightTileKey const departed{ .x = 100, .y = 99 };
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest{
        east, west, departed
    };
    queue.resetPriority({.x = 100, .y = 100}, 127, 0, interest);
    EXPECT_TRUE(queue.push(east));
    EXPECT_TRUE(queue.push(west));
    EXPECT_TRUE(queue.push(departed));
    EXPECT_EQ(queue.top(), departed);

    interest.erase(departed);
    queue.resetPriority({.x = 100, .y = 100}, 127, 0, interest);
    EXPECT_EQ(queue.top(), east);
    EXPECT_EQ(queue.top(), east); // A rejected worker enqueue does not consume the top key.
    queue.resetPriority({.x = 100, .y = 100}, -127, 0, interest);
    EXPECT_EQ(queue.top(), west);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_TRUE(queue.push(departed));
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.push(east));
    EXPECT_EQ(queue.top(), east);
}

TEST(PlayerClientCapturePlanTest, PendingPriorityRefreshIsBoundedAndSurvivesRepeatedHeadingChanges)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr uint32_t KEY_COUNT = 2'048U;
    static constexpr uint32_t REFRESH_BUDGET = 32U;
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
        shared::HeightTileKey const key{
            CENTER.x + static_cast<int32_t>(index % 64U),
            CENTER.y + static_cast<int32_t>(index / 64U),
        };
        interest.insert(key);
        ASSERT_TRUE(queue.push(key));
    }
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> consumed;
    for (uint32_t step = 0U; step < KEY_COUNT; ++step) {
        queue.beginPriorityRefresh(CENTER, step % 2U == 0U ? 127 : -127, 0);
        EXPECT_EQ(queue.size(), KEY_COUNT - consumed.size());
        EXPECT_FALSE(queue.hasReady());
        uint32_t const refreshed = queue.refreshPriority(REFRESH_BUDGET, interest);
        EXPECT_LE(refreshed, REFRESH_BUDGET);
        ASSERT_TRUE(queue.hasReady());
        ASSERT_TRUE(consumed.insert(queue.top()).second);
        queue.pop();
    }
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.hasReady());
    EXPECT_EQ(consumed, interest);
}

TEST(PlayerClientCapturePlanTest, BoundedPriorityRefreshPrunesDeparturesAndKeepsFreshArrivalsReady)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileKey DEPARTED{ 1'999, 2'000 };
    static constexpr shared::HeightTileKey RETAINED{ 2'001, 2'000 };
    static constexpr shared::HeightTileKey ARRIVED{ 2'002, 2'000 };
    static constexpr uint32_t REFRESH_BUDGET = 1U;
    client::PlayerPreviewMeshQueue queue;
    ASSERT_TRUE(queue.push(DEPARTED));
    ASSERT_TRUE(queue.push(RETAINED));
    queue.beginPriorityRefresh(CENTER, 127, 0);
    ASSERT_TRUE(queue.push(ARRIVED));
    EXPECT_TRUE(queue.hasReady());
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{RETAINED, ARRIVED};
    EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
    EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.top(), RETAINED);
    queue.pop();
    EXPECT_EQ(queue.top(), ARRIVED);
    queue.pop();
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest, MovingCenterAdmitsNearbyPendingMeshesAheadOfOldPriorityBacklog)
{
    static constexpr shared::HeightTileKey INITIAL_CENTER{ 2'048, 2'048 };
    static constexpr uint32_t REFRESH_BUDGET = 256U;
    static constexpr int32_t NEAR_RADIUS = 7;
    static constexpr std::array<shared::HeightTileKey, 8> MOVED_CENTERS{{
        { 1'848, 2'048 }, { 2'248, 2'048 }, { 1'868, 2'048 }, { 2'228, 2'048 },
        { 1'888, 2'048 }, { 2'208, 2'048 }, { 1'908, 2'048 }, { 2'188, 2'048 },
    }};
    shared::HeightTileInterest const initial_interest = shared::makeHeightTileInterest(
        INITIAL_CENTER, 127, 0, shared::HEIGHT_TILE_INTEREST_RADIUS
    );
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        initial_interest.keys.begin(), initial_interest.keys.end()
    };
    client::PlayerPreviewMeshQueue queue;
    queue.resetPriority(INITIAL_CENTER, 127, 0, interest);
    for (shared::HeightTileKey const key : initial_interest.keys) {
        ASSERT_TRUE(queue.push(key));
    }

    for (uint32_t step = 0U; step < static_cast<uint32_t>(MOVED_CENTERS.size()); ++step) {
        shared::HeightTileKey const center = MOVED_CENTERS[step];
        ASSERT_TRUE(interest.contains(center)) << "motion step " << step;
        int8_t const heading_x = step % 2U == 0U ? -127 : 127;
        queue.beginPriorityRefresh(center, heading_x, 0);
        ASSERT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
        std::array<shared::HeightTileKey, 225U> nearby_candidates{};
        uint32_t candidate_count = 0U;
        for (int32_t y = -NEAR_RADIUS; y <= NEAR_RADIUS; ++y) {
            for (int32_t x = -NEAR_RADIUS; x <= NEAR_RADIUS; ++x) {
                nearby_candidates[candidate_count++] = { center.x + x, center.y + y };
            }
        }
        std::span<shared::HeightTileKey const> const candidates{ nearby_candidates };
        EXPECT_TRUE(queue.contains(center)) << "motion step " << step;
        EXPECT_EQ(queue.nearestPending(candidates), center) << "motion step " << step;
        EXPECT_EQ(queue.nearestPending(candidates), center) << "unaccepted enqueue consumed pending key";
        EXPECT_TRUE(queue.erase(center)) << "motion step " << step;
        EXPECT_FALSE(queue.contains(center)) << "accepted enqueue should consume pending key";
    }
}

TEST(PlayerClientCapturePlanTest, RequeuedKeyDoesNotResurfaceFromItsStaleHeapEntry)
{
    static constexpr shared::HeightTileKey CENTER{ 2'048, 2'048 };
    static constexpr shared::HeightTileKey KEY{ 2'049, 2'048 };
    static constexpr uint32_t REFRESH_BUDGET = 1U;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{ KEY };
    client::PlayerPreviewMeshQueue queue;
    ASSERT_TRUE(queue.push(KEY));
    queue.beginPriorityRefresh(CENTER, 127, 0);
    ASSERT_TRUE(queue.erase(KEY));
    ASSERT_TRUE(queue.push(KEY));
    ASSERT_TRUE(queue.contains(KEY));
    ASSERT_TRUE(queue.hasReady());
    EXPECT_EQ(queue.top(), KEY);
    queue.pop();
    EXPECT_FALSE(queue.contains(KEY));
    ASSERT_TRUE(queue.empty());

    EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.hasReady());
}

TEST(PlayerClientCapturePlanTest, NearbyPendingProbeWrapsAcrossTheWorldEdge)
{
    static constexpr shared::HeightTileKey CENTER{ 4'093, 2'048 };
    static constexpr shared::HeightTileKey WRAPPED_EAST{ 0, 2'048 };
    static constexpr int32_t NEAR_RADIUS = 7;
    std::array<shared::HeightTileKey, 225U> candidates{};
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    uint32_t candidate_count = 0U;
    for (int32_t y = -NEAR_RADIUS; y <= NEAR_RADIUS; ++y) {
        for (int32_t x = -NEAR_RADIUS; x <= NEAR_RADIUS; ++x) {
            shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                .x = CENTER.x + x,
                .y = CENTER.y + y,
            });
            candidates[candidate_count++] = key;
            interest.insert(key);
        }
    }
    client::PlayerPreviewMeshQueue queue;
    queue.resetPriority(CENTER, 127, 0, interest);
    for (shared::HeightTileKey const key : candidates) {
        ASSERT_TRUE(queue.push(key));
    }
    std::span<shared::HeightTileKey const> const candidate_span{ candidates };

    EXPECT_TRUE(interest.contains(WRAPPED_EAST));
    EXPECT_EQ(queue.nearestPending(candidate_span), CENTER);
}

TEST(PlayerClientCapturePlanTest, MaintainedCoverageMatchesVisibleInterestThroughPublicationAndReset)
{
    static constexpr uint32_t KEY_COUNT = 32U;
    static constexpr uint32_t STEPS = 1'024U;
    static constexpr uint32_t INITIAL_SEED = 0x5EEDU;
    client::PlayerPreviewMeshCoverage coverage;
    std::unordered_set<uint32_t> interest;
    std::unordered_set<uint32_t> visible;
    uint32_t seed = INITIAL_SEED;
    for (uint32_t step = 0U; step < STEPS; ++step) {
        seed = seed * 1'664'525U + 1'013'904'223U;
        uint32_t const key = (seed >> 8U) % KEY_COUNT;
        switch (seed % 6U) {
        case 0U:
            if (interest.insert(key).second) {
                coverage.interestAdded(visible.contains(key));
            }
            break;
        case 1U:
            coverage.meshPublished(visible.insert(key).second, interest.contains(key));
            break;
        case 2U:
            if (interest.erase(key) > 0U) {
                coverage.interestRemoved(visible.contains(key));
            }
            break;
        case 3U:
            if (!interest.contains(key)) {
                visible.erase(key);
            }
            break;
        case 4U:
            if (visible.contains(key)) {
                coverage.meshPublished(false, interest.contains(key));
            }
            break;
        case 5U:
            if (step % 31U == 0U) {
                coverage.clear();
                interest.clear();
                visible.clear();
            }
            break;
        default:
            FAIL() << "LCG event is outside the expected six cases";
        }
        uint32_t expected_visible = 0U;
        for (uint32_t const interested : interest) {
            if (visible.contains(interested)) {
                ++expected_visible;
            }
        }
        EXPECT_EQ(coverage.visibleCount(), expected_visible) << step;
        EXPECT_EQ(
            coverage.complete(static_cast<uint32_t>(interest.size())),
            !interest.empty() && expected_visible == interest.size()
        ) << step;
    }
    for (uint32_t const key : interest) {
        coverage.meshPublished(visible.insert(key).second, true);
    }
    ASSERT_FALSE(interest.empty());
    EXPECT_TRUE(coverage.complete(static_cast<uint32_t>(interest.size())));
    coverage.clear();
    EXPECT_EQ(coverage.visibleCount(), 0U);
    EXPECT_FALSE(coverage.complete(0U));
}
