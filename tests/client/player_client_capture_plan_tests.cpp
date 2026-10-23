#include <client/Camera.hpp>
#include <client/HeightTileInterestRegion.hpp>
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

#include <testsupport/TerrainFixtures.hpp>

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
#include <ratio>
#include <span>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

client::HeightTileReplacementResult acceptTerrainTile(
    client::PreviewResidency& residency,
    shared::HeightTileKey const key,
    client::HeightTileRevision const revision,
    uint64_t const token,
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const& heights
)
{
    return residency.accept(
        key,
        revision,
        token,
        heights,
        testsupport::makeSurfaceTile(
            key,
            testsupport::makeTerrainIdentity(42U, revision.revision),
            heights
        )
    );
}

} // namespace

TEST(PlayerClientCapturePlanTest, FramesTheCentralSpikeAndFirstRingTogether) {
  static constexpr int32_t WORLD_CENTER = 32'768;
  static constexpr int32_t PREVIOUS_PLAYER_DISTANCE = 243;
  static constexpr int32_t PLAYER_DISTANCE = 170;
  static constexpr int32_t PREVIOUS_PLAYER_HEIGHT = 440;
  static constexpr int32_t PLAYER_HEIGHT = 420;
  static constexpr int32_t FIRST_RING_RADIUS = 128;
  static constexpr uint32_t FRAME_WIDTH = 2560U;
  static constexpr uint32_t FRAME_HEIGHT = 1440U;
  client::PlayerClientCapturePlan const plan = client::playerClientCapturePlan(
      client::PlayerClientCapturePreset::CentralSpike);
  EXPECT_EQ(plan.target.x, WORLD_CENTER - PLAYER_DISTANCE);
  EXPECT_EQ(plan.target.y, WORLD_CENTER);
  EXPECT_EQ(plan.target.z, PLAYER_HEIGHT);
  EXPECT_NEAR(static_cast<double>(PREVIOUS_PLAYER_DISTANCE - PLAYER_DISTANCE) /
                  PREVIOUS_PLAYER_DISTANCE,
              0.30, 0.01);
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
  client::Camera const look_camera{
      {.position = eye, .angles = plan.look_angles}};
  client::Camera const camera{
      {.position = eye - look_camera.forward() * plan.rear_camera_distance,
       .angles = plan.look_angles},
      {.vertical_fov_degrees = plan.vertical_fov_degrees},
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
      static_cast<float>(WORLD_CENTER),
      static_cast<float>(WORLD_CENTER),
      static_cast<float>(terrain.heightAt(WORLD_CENTER, WORLD_CENTER)),
      1.0F,
  });
  glm::vec3 const near_ring = projected({
      static_cast<float>(near_ring_x),
      static_cast<float>(WORLD_CENTER),
      static_cast<float>(near_ring_height),
      1.0F,
  });
  glm::vec3 const far_ring = projected({
      static_cast<float>(far_ring_x),
      static_cast<float>(WORLD_CENTER),
      static_cast<float>(far_ring_height),
      1.0F,
  });
  static constexpr int32_t BACKGROUND_RADIUS = FIRST_RING_RADIUS * 2;
  glm::vec4 const background_clip = clip_coordinates({
      static_cast<float>(WORLD_CENTER),
      static_cast<float>(WORLD_CENTER - BACKGROUND_RADIUS),
      static_cast<float>(
          terrain.heightAt(WORLD_CENTER, WORLD_CENTER - BACKGROUND_RADIUS)),
      1.0F,
  });
  glm::vec3 const background = glm::vec3(background_clip) / background_clip.w;
  glm::dvec3 const camera_position = camera.pose().position;
  double const segment_x =
      static_cast<double>(WORLD_CENTER) - camera_position.x;
  double const segment_y =
      static_cast<double>(WORLD_CENTER - BACKGROUND_RADIUS) - camera_position.y;
  double const camera_radius_x = camera_position.x - WORLD_CENTER;
  double const camera_radius_y = camera_position.y - WORLD_CENTER;
  double const nearest_segment_parameter =
      std::clamp(-(camera_radius_x * segment_x + camera_radius_y * segment_y) /
                     (segment_x * segment_x + segment_y * segment_y),
                 0.0, 1.0);
  double const nearest_background_sightline_radius =
      std::hypot(camera_radius_x + nearest_segment_parameter * segment_x,
                 camera_radius_y + nearest_segment_parameter * segment_y);
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
  EXPECT_GT(terrain.heightAt(WORLD_CENTER, WORLD_CENTER - BACKGROUND_RADIUS),
            0U);
  EXPECT_GT(nearest_background_sightline_radius, FIRST_RING_RADIUS);
  EXPECT_GT(background.x, 0.25F);
  EXPECT_LT(background.x, 0.8F);
  EXPECT_GT(background.y, 0.55F);
  EXPECT_LT(background.y, 0.85F);
}

TEST(PlayerClientCapturePlanTest, FramesTheUpperFirstRingAsAnAscendingSlope) {
  static constexpr int32_t WORLD_CENTER = 32'768;
  static constexpr int32_t FIRST_RING_ASCENT_RADIUS = 112;
  static constexpr int32_t ASCENT_SAMPLE_DISTANCE = 16;
  static constexpr uint16_t MINIMUM_ASCENT = 50U;
  static constexpr double MINIMUM_CLIMBING_PITCH = 45.0;
  static constexpr double MAXIMUM_CLIMBING_PITCH = 65.0;
  static constexpr double MINIMUM_ASCENT_YAW_OFFSET = 5.0;
  static constexpr double MAXIMUM_ASCENT_YAW_OFFSET = 20.0;
  client::PlayerClientCapturePlan const plan = client::playerClientCapturePlan(
      client::PlayerClientCapturePreset::MountainClimb);
  shared::TerrainGenerator const terrain;
  uint16_t const target_height = terrain.heightAt(plan.target.x, plan.target.y);

  EXPECT_EQ(plan.target.x, WORLD_CENTER + FIRST_RING_ASCENT_RADIUS);
  EXPECT_EQ(plan.target.y, WORLD_CENTER);
  EXPECT_EQ(plan.target.z, target_height);
  EXPECT_LT(
      terrain.heightAt(plan.target.x - ASCENT_SAMPLE_DISTANCE, plan.target.y),
      target_height);
  EXPECT_GE(
      terrain.heightAt(plan.target.x + ASCENT_SAMPLE_DISTANCE, plan.target.y) -
          target_height,
      MINIMUM_ASCENT);
  double const ascent_yaw_offset =
      std::abs(plan.look_angles.yaw_degrees - 90.0);
  EXPECT_GE(ascent_yaw_offset, MINIMUM_ASCENT_YAW_OFFSET);
  EXPECT_LE(ascent_yaw_offset, MAXIMUM_ASCENT_YAW_OFFSET);
  EXPECT_GE(plan.look_angles.pitch_degrees, MINIMUM_CLIMBING_PITCH);
  EXPECT_LE(plan.look_angles.pitch_degrees, MAXIMUM_CLIMBING_PITCH);
}

TEST(PlayerClientCapturePlanTest,
     RequiresMountainCaptureWithBothMovementCapabilitiesOff) {
  client::PlayerClientCapturePlan const plan = client::playerClientCapturePlan(
      client::PlayerClientCapturePreset::MountainClimb);

  ASSERT_TRUE(plan.final_movement_capabilities.has_value());
  EXPECT_EQ(plan.final_movement_capabilities->bits, 0U);
}

TEST(PlayerClientCapturePlanTest,
     BoundsFullTerrainCaptureReadinessToFifteenMinutes) {
  client::PlayerClientCaptureOptions const options{};

  EXPECT_EQ(options.readiness_deadline, std::chrono::minutes{15});
  EXPECT_EQ(options.sweep_deadline, std::chrono::minutes{5});
  EXPECT_EQ(options.minimum_height_tile_meshes, 0U);
}

TEST(PlayerClientCapturePlanTest,
     GivesArenaGrowthTimeThroughoutCapturePreload) {
  EXPECT_EQ(client::playerClientRendererFrameBudget(true),
            std::chrono::seconds{1});
  EXPECT_EQ(client::playerClientRendererFrameBudget(false),
            std::chrono::milliseconds{8});
}

TEST(PlayerClientCapturePlanTest, PresentationDeadlineUsesTheWhole120HzPeriodWithoutRestartingAfterSetup)
{
    static constexpr std::chrono::nanoseconds PERIOD{ 8'333'334 };
    static constexpr std::chrono::nanoseconds ONE_TICK{ 1 };
    auto const started = std::chrono::steady_clock::time_point{};
    auto const deadline = client::playerClientPresentationDeadline(started, false);
    EXPECT_EQ(deadline - started, PERIOD);
    auto const formerly_expired = started + std::chrono::milliseconds{ 8 };
    EXPECT_GT(deadline, formerly_expired);
    EXPECT_EQ(deadline - (started + PERIOD - ONE_TICK), ONE_TICK);
    EXPECT_EQ(deadline - (started + PERIOD), std::chrono::nanoseconds::zero());
    EXPECT_LT(deadline, started + PERIOD + ONE_TICK);
    EXPECT_EQ(client::playerClientPresentationDeadline(started, true) - started, std::chrono::seconds{ 1 });
    EXPECT_EQ(client::playerClientRendererFrameBudget(false), std::chrono::milliseconds{ 8 });
    EXPECT_EQ(client::playerClientPreviewMeshWorkDeadline(
        started + std::chrono::milliseconds{ 8 }, started + std::chrono::milliseconds{ 9 }),
        started + std::chrono::milliseconds{ 10 });
}

TEST(PlayerClientCapturePlanTest,
     ReservesOnlyOneFreshMillisecondOfMeshWorkAfterInterestRefreshOverruns) {
  static constexpr std::chrono::milliseconds RENDERER_BUDGET{8};
  static constexpr std::chrono::milliseconds OVERRUN{3};
  static constexpr std::chrono::milliseconds MESH_WORK_RESERVE{1};
  std::chrono::steady_clock::time_point const frame_started_at{};
  std::chrono::steady_clock::time_point const renderer_deadline =
      frame_started_at + RENDERER_BUDGET;
  std::chrono::steady_clock::time_point const refresh_completed_at =
      renderer_deadline + OVERRUN;
  std::chrono::steady_clock::time_point const mesh_work_deadline =
      client::playerClientPreviewMeshWorkDeadline(renderer_deadline, refresh_completed_at);

  EXPECT_EQ(mesh_work_deadline, refresh_completed_at + MESH_WORK_RESERVE);
  EXPECT_EQ(mesh_work_deadline - refresh_completed_at, MESH_WORK_RESERVE);
  EXPECT_GT(mesh_work_deadline, renderer_deadline);
}

TEST(PlayerClientCapturePlanTest,
     KeepsExistingRendererDeadlineWhenInterestRefreshCompletesWithinBudget) {
  static constexpr std::chrono::milliseconds RENDERER_BUDGET{8};
  static constexpr std::chrono::milliseconds REFRESH_DURATION{3};
  std::chrono::steady_clock::time_point const frame_started_at{};
  std::chrono::steady_clock::time_point const renderer_deadline =
      frame_started_at + RENDERER_BUDGET;
  std::chrono::steady_clock::time_point const refresh_completed_at =
      frame_started_at + REFRESH_DURATION;

  EXPECT_EQ(client::playerClientPreviewMeshWorkDeadline(
                renderer_deadline, refresh_completed_at),
            renderer_deadline);
}

TEST(PlayerClientCapturePlanTest, ProbesNearMeshesFromTheCenterOut) {
  std::array<client::PlayerClientPreviewMeshProbeOffset,
             client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT>
      visited{};
  int32_t previous_radius = 0;
  for (size_t index = 0U; index < visited.size(); ++index) {
    client::PlayerClientPreviewMeshProbeOffset const offset =
        client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS[index];
    int32_t const radius = std::max(std::abs(offset.x), std::abs(offset.y));
    EXPECT_GE(radius, previous_radius) << "probe index " << index;
    EXPECT_LE(radius, client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS);
    EXPECT_EQ(
        std::find_if(
            visited.begin(),
            visited.begin() + index,
            [&](auto const previous) {
              return previous.x == offset.x && previous.y == offset.y;
            }),
        visited.begin() + index)
        << "duplicate probe offset at index " << index;
    visited[index] = offset;
    previous_radius = radius;
  }

  EXPECT_EQ(previous_radius,
            client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS);
}

TEST(PlayerClientCapturePlanTest, DrainsFullReceiveWindowDuringCapturePreload) {
  EXPECT_EQ(client::playerClientHeightTileChangeBudget(true),
            client::playerClientHeightTileChangeBudget(false));
}

TEST(PlayerClientCapturePlanTest,
     UsesTheSamePreviewMeshJobBudgetDuringCaptureAndPlay) {
  EXPECT_EQ(client::playerClientPreviewMeshJobBudget(true),
            client::playerClientPreviewMeshJobBudget(false));
}

TEST(PlayerClientCapturePlanTest,
     DrainsOneBoundedProtocolWindowOfResidencyChangesPerFrame) {
  static constexpr uint32_t PROTOCOL_WINDOW =
      shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY * shared::HEIGHT_TILE_DELIVERY_WINDOW;
  static constexpr uint32_t CHANGE_COUNT = PROTOCOL_WINDOW + 1U;
  static constexpr client::HeightTileRevision REVISION{
      .generation = 1U,
      .revision = 1U,
  };
  std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();
  client::PreviewResidency residency{REVISION};

  for (uint32_t index = 0U; index < CHANGE_COUNT; ++index) {
    shared::HeightTileKey const key{static_cast<int32_t>(index), 1};
    ASSERT_EQ(
        acceptTerrainTile(residency, key, REVISION, static_cast<uint64_t>(index) + 1U, heights).replacement,
        client::HeightTileReplacement::Published);
  }

  std::vector<client::HeightTileChange> const first_window =
      residency.takeChanges(client::playerClientHeightTileChangeBudget(false));
  ASSERT_EQ(first_window.size(), PROTOCOL_WINDOW);
  EXPECT_EQ(residency.pendingChangeCount(), 1U);
  EXPECT_EQ(first_window.front().key.x, 0);
  EXPECT_EQ(first_window.back().key.x,
            static_cast<int32_t>(PROTOCOL_WINDOW - 1U));

  std::vector<client::HeightTileChange> const second_window =
      residency.takeChanges(client::playerClientHeightTileChangeBudget(false));
  ASSERT_EQ(second_window.size(), 1U);
  EXPECT_EQ(second_window.front().key.x, static_cast<int32_t>(PROTOCOL_WINDOW));
  EXPECT_EQ(residency.pendingChangeCount(), 0U);
}

TEST(PlayerClientCapturePlanTest,
     ConsumesMixedReplacementReceiptsWithoutRetainingAnArtificialHandoffBacklog) {
  static constexpr uint32_t WINDOW_OPERATIONS =
      shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY * shared::HEIGHT_TILE_DELIVERY_WINDOW;
  static constexpr uint32_t REPLACEMENT_COUNT = WINDOW_OPERATIONS / 2U;
  static constexpr client::HeightTileRevision REVISION{1U, 1U};
  std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();
  for (bool const capture_enabled : {false, true}) {
    client::PreviewResidency residency{REVISION};
    for (uint32_t index = 0U; index < REPLACEMENT_COUNT; ++index) {
      client::HeightTileKey const key{static_cast<int32_t>(index), 0};
      ASSERT_EQ(acceptTerrainTile(residency, key, REVISION, 1U, heights).replacement,
                client::HeightTileReplacement::Published);
    }
    ASSERT_EQ(residency.takeChanges(WINDOW_OPERATIONS).size(), REPLACEMENT_COUNT);
    for (uint32_t index = 0U; index < REPLACEMENT_COUNT; ++index) {
      client::HeightTileKey const old_key{static_cast<int32_t>(index), 0};
      client::HeightTileKey const new_key{static_cast<int32_t>(index), 1};
      ASSERT_TRUE(residency.evict(old_key, REVISION, 2U));
      ASSERT_EQ(acceptTerrainTile(residency, new_key, REVISION, 3U, heights).replacement,
                client::HeightTileReplacement::Published);
    }
    ASSERT_EQ(residency.pendingChangeCount(), WINDOW_OPERATIONS);
    std::vector<client::HeightTileChange> const changes = residency.takeChanges(
        client::playerClientHeightTileChangeBudget(capture_enabled));
    auto const removed = std::ranges::count(
        changes, client::HeightTileChangeKind::Remove, &client::HeightTileChange::kind);
    auto const upserted = std::ranges::count(
        changes, client::HeightTileChangeKind::Upsert, &client::HeightTileChange::kind);
    EXPECT_EQ(removed, REPLACEMENT_COUNT);
    EXPECT_EQ(upserted, REPLACEMENT_COUNT);
    EXPECT_EQ(residency.pendingChangeCount(), 0U);
    EXPECT_EQ(residency.stats().resident_tiles, REPLACEMENT_COUNT);
  }
}

TEST(PlayerClientCapturePlanTest,
     WorkerAdmissionWindowBoundsOutstandingJobsAndReopensOnConsumption) {
  static constexpr uint32_t PLAY_CAPACITY = 128U;
  static constexpr uint32_t CAPTURE_CAPACITY = 16U;
  EXPECT_EQ(client::playerClientPreviewMeshAdmissionBudget(false),
            PLAY_CAPACITY);
  EXPECT_EQ(client::playerClientPreviewMeshAdmissionBudget(true),
            CAPTURE_CAPACITY);
  client::PlayerPreviewMeshAdmissionWindow play_window{
      client::playerClientPreviewMeshBacklogCapacity(false)};
  client::PlayerPreviewMeshAdmissionWindow capture_window{
      client::playerClientPreviewMeshBacklogCapacity(true)};

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
  for (uint32_t index = 0U; index < CAPTURE_CAPACITY; ++index) {
    ASSERT_TRUE(capture_window.releaseOne()) << index;
  }
  EXPECT_FALSE(capture_window.releaseOne());
  EXPECT_EQ(capture_window.outstandingCount(), 0U);
  ASSERT_TRUE(capture_window.tryAdmit());
  EXPECT_EQ(capture_window.outstandingCount(), 1U);
}

TEST(PlayerClientCapturePlanTest,
     DepartedPreviewRemovalsProgressBeforeCoverageAndKeepRestoredKeys) {
  static constexpr uint32_t PROTOCOL_WINDOW =
      shared::HEIGHT_TILE_DELIVERY_BATCH_CAPACITY * shared::HEIGHT_TILE_DELIVERY_WINDOW;
  static constexpr uint32_t RESTORED_KEY = PROTOCOL_WINDOW / 2U;
  EXPECT_FALSE(client::playerClientShouldQueuePreviewRemoval(true, false));
  EXPECT_TRUE(client::playerClientShouldQueuePreviewRemoval(true, true));
  EXPECT_TRUE(client::playerClientShouldQueuePreviewRemoval(false, false));

  std::deque<uint32_t> pending;
  for (uint32_t index = 0U; index <= PROTOCOL_WINDOW; ++index) {
    pending.push_back(index);
  }
  std::unordered_set<uint32_t> current_interest{RESTORED_KEY};
  std::vector<uint32_t> removed;
  uint32_t const budget = client::playerClientPreviewRemovalBudget();
  for (uint32_t processed = 0U; processed < budget && !pending.empty();
       ++processed) {
    uint32_t const key = pending.front();
    pending.pop_front();
    if (client::playerClientShouldRemovePreviewMesh(
            current_interest.contains(key))) {
      removed.push_back(key);
    }
  }

  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front(), PROTOCOL_WINDOW);
  ASSERT_EQ(removed.size(), PROTOCOL_WINDOW - 1U);
  EXPECT_EQ(removed.front(), 0U);
  EXPECT_EQ(removed.back(), PROTOCOL_WINDOW - 1U);
  EXPECT_EQ(std::ranges::find(removed, RESTORED_KEY), removed.end());
  EXPECT_FALSE(client::playerClientShouldRemovePreviewMesh(
      current_interest.contains(RESTORED_KEY)));
}

TEST(PlayerClientCapturePlanTest,
     RequestsPhysicalHighResolutionAtOneAndTwoTimesScale) {
  EXPECT_EQ(client::playerClientCaptureLogicalSize(1280U, 1280U, 2560U), 2560U);
  EXPECT_EQ(client::playerClientCaptureLogicalSize(720U, 720U, 1440U), 1440U);
  EXPECT_EQ(client::playerClientCaptureLogicalSize(1280U, 2560U, 2560U), 1280U);
  EXPECT_EQ(client::playerClientCaptureLogicalSize(720U, 1440U, 1440U), 720U);
}

TEST(PlayerClientCapturePlanTest, EnforcesThePortableCaptureExtentFloor) {
  static constexpr uint32_t MINIMUM_WIDTH = 1'000U;
  static constexpr uint32_t MINIMUM_HEIGHT = 600U;
  EXPECT_TRUE(client::playerClientCaptureExtentMeetsMinimum(
      MINIMUM_WIDTH,
      MINIMUM_HEIGHT));
  EXPECT_TRUE(client::playerClientCaptureExtentMeetsMinimum(2560U, 1440U));
  EXPECT_FALSE(client::playerClientCaptureExtentMeetsMinimum(
      MINIMUM_WIDTH - 1U,
      MINIMUM_HEIGHT));
  EXPECT_FALSE(client::playerClientCaptureExtentMeetsMinimum(
      MINIMUM_WIDTH, MINIMUM_HEIGHT - 1U));
}

TEST(PlayerClientCapturePlanTest,
     NearTargetNavigationUsesAProtocolValidSpeedup) {
  static constexpr int64_t NEAR_DISTANCE_SUBCELLS = 40'000;
  uint16_t const speedup =
      client::playerClientCaptureSpeedup(NEAR_DISTANCE_SUBCELLS);
  ASSERT_TRUE(shared::isFlightSpeedupProfile(speedup));
  EXPECT_EQ(speedup, 2U);

  shared::ClientInputMessage const input{
      .direction = {.x = 1U, .y = 0U, .accelerated = false, .speedup = speedup},
      .sequence = 1U,
  };
  auto const decoded = shared::decodeMessage(shared::encodeMessage(input));
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<shared::ClientInputMessage>(*decoded));
  EXPECT_EQ(std::get<shared::ClientInputMessage>(*decoded).direction.speedup,
            speedup);
}

TEST(PlayerClientCapturePlanTest,
     StaleVisibleMeshCannotSatisfyResidentCoverage) {
  static constexpr uint32_t EXPECTED_TILES = 1U;
  static constexpr shared::HeightTileKey KEY{.x = 1, .y = 1};
  static constexpr client::HeightTileRevision REVISION{
      .generation = 1U,
      .revision = 1U,
  };
  client::PreviewResidency residency{REVISION};
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{KEY};
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const visible_meshes{KEY};
  std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();

  ASSERT_EQ(acceptTerrainTile(residency, KEY, REVISION, 1U, heights).replacement,
            client::HeightTileReplacement::Published);
  EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
      interest, visible_meshes, residency, EXPECTED_TILES, 0U));
  static_cast<void>(residency.takeChanges(1U));
  EXPECT_TRUE(client::playerClientCaptureCoverageComplete(
      interest, visible_meshes, residency, EXPECTED_TILES, 0U));
  EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
      interest, visible_meshes, residency, EXPECTED_TILES, 1U));

  ASSERT_TRUE(residency.evict(KEY, REVISION, 2U));
  static_cast<void>(residency.takeChanges(1U));
  EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
      interest, visible_meshes, residency, EXPECTED_TILES, 0U));
}

TEST(PlayerClientCapturePlanTest, GeometricCaptureCoverageRequiresEveryResidentDiskTile)
{
    static constexpr uint32_t RADIUS = 1U;
    static constexpr uint32_t EXPECTED_TILES = shared::heightTileInterestCount(RADIUS);
    static constexpr shared::HeightTileKey CENTER{ .x = 4'095, .y = 4'095 };
    static constexpr client::HeightTileRevision REVISION{ .generation = 1U, .revision = 1U };
    client::HeightTileInterestRegion interest;
    interest.set(CENTER, RADIUS);
    client::PreviewResidency residency{ REVISION };
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> visible_meshes;
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> const heights = testsupport::makeHeightSamples();
    uint32_t covered_count = 0U;

    for (int32_t y = -static_cast<int32_t>(RADIUS); y <= static_cast<int32_t>(RADIUS); ++y) {
        for (int32_t x = -static_cast<int32_t>(RADIUS); x <= static_cast<int32_t>(RADIUS); ++x) {
            shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                .x = CENTER.x + x,
                .y = CENTER.y + y,
            });
            if (!interest.contains(key)) {
                continue;
            }
            ASSERT_EQ(acceptTerrainTile(residency, key, REVISION, 1U, heights).replacement,
                client::HeightTileReplacement::Published);
            visible_meshes.insert(key);
            ++covered_count;
        }
    }
    ASSERT_EQ(covered_count, EXPECTED_TILES);
    static_cast<void>(residency.takeChanges(EXPECTED_TILES));

    EXPECT_TRUE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 0U));
    EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 1U));
    visible_meshes.erase(shared::normalizeHeightTileKey(CENTER));
    EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 0U));
    interest.clear();
    EXPECT_FALSE(client::playerClientCaptureCoverageComplete(
        interest, visible_meshes, residency, EXPECTED_TILES, 0U));
}

TEST(PlayerClientCapturePlanTest, GeometricInterestKeepsNearAndForecastQueueEntries)
{
    static constexpr shared::HeightTileKey CENTER{ .x = 2'048, .y = 2'048 };
    static constexpr shared::HeightTileKey NEAR{ .x = 2'049, .y = 2'048 };
    static constexpr shared::HeightTileKey FORECAST{ .x = 2'060, .y = 2'048 };
    static constexpr shared::HeightTileKey OUTSIDE{ .x = 2'121, .y = 2'048 };
    static constexpr uint32_t RADIUS = 72U;
    static constexpr uint32_t REFRESH_BUDGET = 3U;
    client::HeightTileInterestRegion interest;
    interest.set(CENTER, RADIUS);
    client::PlayerPreviewMeshQueue queue;
    queue.beginPriorityRefresh(CENTER, 127, 0);
    std::array const keys{ NEAR, FORECAST, OUTSIDE };
    queue.pushBatch(keys);

    EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.top(), NEAR);
    queue.pop();
    EXPECT_EQ(queue.top(), FORECAST);
    queue.pop();
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest,
     PendingMeshesPopByStablePriorityWithoutDuplicates) {
  shared::HeightTileKey const center{.x = 100, .y = 100};
  std::vector<shared::HeightTileKey> keys{
      {.x = 120, .y = 100}, {.x = 100, .y = 120}, {.x = 80, .y = 100},
      {.x = 100, .y = 80},  {.x = 101, .y = 100},
  };
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{
      keys.begin(), keys.end()};
  client::PlayerPreviewMeshQueue queue;
  queue.resetPriority(center, 127, 0, interest);
  for (shared::HeightTileKey const key : keys) {
    EXPECT_TRUE(queue.push(key));
    EXPECT_FALSE(queue.push(key));
  }
  EXPECT_EQ(queue.size(), keys.size());
  std::ranges::sort(
      keys,
      [center](shared::HeightTileKey const first,
               shared::HeightTileKey const second) {
        return std::tuple{
                   shared::heightTileInterestPriority(center, 127, 0, first),
                   first.y,
                   first.x,
               } < std::tuple{
                   shared::heightTileInterestPriority(center, 127, 0, second),
                   second.y,
                   second.x,
               };
      });
  for (shared::HeightTileKey const key : keys) {
    EXPECT_EQ(queue.top(), key);
    queue.pop();
  }
  EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest,
     BatchAdmissionsDeduplicateAndUseBoundedPriorityRefresh) {
  static constexpr shared::HeightTileKey CENTER{100, 100};
  static constexpr shared::HeightTileKey DEPARTED{99, 100};
  std::array<shared::HeightTileKey, 5U> const keys{{
      {120, 100},
      CENTER,
      {100, 101},
      DEPARTED,
      CENTER,
  }};
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{
      keys[0],
      keys[1],
      keys[2],
  };
  client::PlayerPreviewMeshQueue queue;
  queue.resetPriority(CENTER, 127, 0, interest);
  queue.pushBatch(keys);

  EXPECT_EQ(queue.size(), 4U);
  EXPECT_FALSE(queue.hasReady());
  std::array<shared::HeightTileKey, 3U> const nearby_candidates{
      {keys[0], keys[1], keys[2]}};
  EXPECT_EQ(queue.nearestPending(nearby_candidates), CENTER);
  EXPECT_EQ(queue.refreshPriority(2U, interest), 2U);
  EXPECT_TRUE(queue.hasReady());
  EXPECT_EQ(queue.refreshPriority(2U, interest), 2U);
  EXPECT_TRUE(queue.hasReady());
  EXPECT_EQ(queue.size(), interest.size());

  std::vector<shared::HeightTileKey> expected{interest.begin(), interest.end()};
  std::ranges::sort(
      expected,
      [](shared::HeightTileKey const first,
         shared::HeightTileKey const second) {
        return std::tuple{
                   shared::heightTileInterestPriority(CENTER, 127, 0, first),
                   first.y,
                   first.x,
               } < std::tuple{
                   shared::heightTileInterestPriority(CENTER, 127, 0, second),
                   second.y,
                   second.x,
               };
      });
  for (shared::HeightTileKey const key : expected) {
    EXPECT_EQ(queue.top(), key);
    queue.pop();
  }
  EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest,
     PendingMeshesReprioritizePruneAndRetainFailedEnqueueTop) {
  shared::HeightTileKey const east{.x = 140, .y = 100};
  shared::HeightTileKey const west{.x = 60, .y = 100};
  shared::HeightTileKey const departed{.x = 100, .y = 99};
  client::PlayerPreviewMeshQueue queue;
  std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash>
      interest{east, west, departed};
  queue.resetPriority({.x = 100, .y = 100}, 127, 0, interest);
  EXPECT_TRUE(queue.push(east));
  EXPECT_TRUE(queue.push(west));
  EXPECT_TRUE(queue.push(departed));
  EXPECT_EQ(queue.top(), departed);

  interest.erase(departed);
  queue.resetPriority({.x = 100, .y = 100}, 127, 0, interest);
  EXPECT_EQ(queue.top(), east);
  EXPECT_EQ(queue.top(),
            east); // A rejected worker enqueue does not consume the top key.
  queue.resetPriority({.x = 100, .y = 100}, -127, 0, interest);
  EXPECT_EQ(queue.top(), west);
  EXPECT_EQ(queue.size(), 2U);
  EXPECT_TRUE(queue.push(departed));
  queue.clear();
  EXPECT_TRUE(queue.empty());
  EXPECT_TRUE(queue.push(east));
  EXPECT_EQ(queue.top(), east);
}

TEST(PlayerClientCapturePlanTest,
     PendingPriorityRefreshIsBoundedAndSurvivesRepeatedHeadingChanges) {
  static constexpr shared::HeightTileKey CENTER{2'000, 2'000};
  static constexpr uint32_t KEY_COUNT = 2'048U;
  static constexpr uint32_t REFRESH_BUDGET = 32U;
  static constexpr uint32_t MOVEMENT_STEPS = 64U;
  client::PlayerPreviewMeshQueue queue;
  std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash>
      interest;
  for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
    shared::HeightTileKey const key{
        CENTER.x + static_cast<int32_t>(index % 64U),
        CENTER.y + static_cast<int32_t>(index / 64U),
    };
    interest.insert(key);
    ASSERT_TRUE(queue.push(key));
  }
  for (uint32_t step = 0U; step < MOVEMENT_STEPS; ++step) {
    queue.beginPriorityRefresh(CENTER, step % 2U == 0U ? 127 : -127, 0);
    EXPECT_EQ(queue.size(), KEY_COUNT);
    EXPECT_FALSE(queue.hasReady());
    uint32_t const refreshed = queue.refreshPriority(REFRESH_BUDGET, interest);
    EXPECT_EQ(refreshed, REFRESH_BUDGET);
    EXPECT_TRUE(queue.hasReady());
  }

  for (uint32_t remaining = 0U; remaining < KEY_COUNT; remaining += REFRESH_BUDGET) {
    EXPECT_LE(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
  }

  std::vector<shared::HeightTileKey> expected_order{
      interest.begin(),
      interest.end(),
  };
  std::ranges::sort(
      expected_order,
      [](shared::HeightTileKey const first,
         shared::HeightTileKey const second) {
        return std::tuple{
                   shared::heightTileInterestPriority(CENTER, -127, 0, first),
                   first.y,
                   first.x,
               } < std::tuple{
                   shared::heightTileInterestPriority(CENTER, -127, 0, second),
                   second.y,
                   second.x,
               };
      });
  for (shared::HeightTileKey const key : expected_order) {
    EXPECT_EQ(queue.top(), key);
    queue.pop();
  }
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.hasReady());
}

TEST(PlayerClientCapturePlanTest,
     BoundedPriorityRefreshPrunesDeparturesAndKeepsFreshArrivalsReady) {
  static constexpr shared::HeightTileKey CENTER{2'000, 2'000};
  static constexpr shared::HeightTileKey DEPARTED{1'999, 2'000};
  static constexpr shared::HeightTileKey RETAINED{2'001, 2'000};
  static constexpr shared::HeightTileKey ARRIVED{2'002, 2'000};
  static constexpr uint32_t REFRESH_BUDGET = 1U;
  client::PlayerPreviewMeshQueue queue;
  ASSERT_TRUE(queue.push(DEPARTED));
  ASSERT_TRUE(queue.push(RETAINED));
  queue.beginPriorityRefresh(CENTER, 127, 0);
  ASSERT_TRUE(queue.push(ARRIVED));
  EXPECT_TRUE(queue.hasReady());
  EXPECT_EQ(queue.top(), ARRIVED);
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{
      RETAINED,
      ARRIVED,
  };
  EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
  EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
  EXPECT_EQ(queue.size(), 2U);
  EXPECT_EQ(queue.top(), RETAINED);
  queue.pop();
  EXPECT_EQ(queue.top(), ARRIVED);
  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest,
     RefreshedSubsetUsesCurrentCenterBeforeTheWholeBacklogCompletes) {
  static constexpr shared::HeightTileKey INITIAL_CENTER{2'000, 2'000};
  static constexpr shared::HeightTileKey CURRENT_CENTER{2'300, 2'000};
  static constexpr uint32_t KEY_COUNT = 300U;
  static constexpr uint32_t REFRESH_BUDGET = 256U;
  client::PlayerPreviewMeshQueue queue;
  std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash>
      interest;
  for (uint32_t index = 1U; index <= KEY_COUNT; ++index) {
    shared::HeightTileKey const key{
        INITIAL_CENTER.x + static_cast<int32_t>(index), INITIAL_CENTER.y};
    interest.insert(key);
  }
  queue.resetPriority(INITIAL_CENTER, 127, 0, interest);
  for (shared::HeightTileKey const key : interest) {
    ASSERT_TRUE(queue.push(key));
  }
  queue.beginPriorityRefresh(CURRENT_CENTER, 127, 0);

  EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
  ASSERT_TRUE(queue.hasReady());
  ASSERT_TRUE(interest.contains(queue.top()));

  for (uint32_t remaining = 0U; remaining < KEY_COUNT; remaining += REFRESH_BUDGET) {
    EXPECT_LE(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
    EXPECT_TRUE(queue.hasReady());
  }
  EXPECT_EQ(queue.top(), CURRENT_CENTER);
}

TEST(PlayerClientCapturePlanTest,
     MovingCenterAdmitsNearbyPendingMeshesAheadOfOldPriorityBacklog) {
  static constexpr shared::HeightTileKey INITIAL_CENTER{2'048, 2'048};
  static constexpr uint32_t REFRESH_BUDGET = 256U;
  static constexpr int32_t NEAR_RADIUS = 7;
  static constexpr std::array<shared::HeightTileKey, 8> MOVED_CENTERS{{
      {1'848, 2'048},
      {2'248, 2'048},
      {1'868, 2'048},
      {2'228, 2'048},
      {1'888, 2'048},
      {2'208, 2'048},
      {1'908, 2'048},
      {2'188, 2'048},
  }};
  shared::HeightTileInterest const initial_interest =
      shared::makeHeightTileInterest(
          INITIAL_CENTER,
          127,
          0,
          shared::HEIGHT_TILE_INTEREST_RADIUS);
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{
      initial_interest.keys.begin(), initial_interest.keys.end()};
  client::PlayerPreviewMeshQueue queue;
  queue.resetPriority(INITIAL_CENTER, 127, 0, interest);
  for (shared::HeightTileKey const key : initial_interest.keys) {
    ASSERT_TRUE(queue.push(key));
  }

  for (uint32_t step = 0U; step < static_cast<uint32_t>(MOVED_CENTERS.size());
       ++step) {
    shared::HeightTileKey const center = MOVED_CENTERS[step];
    ASSERT_TRUE(interest.contains(center)) << "motion step " << step;
    int8_t const heading_x = step % 2U == 0U ? -127 : 127;
    queue.beginPriorityRefresh(center, heading_x, 0);
    ASSERT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
    std::array<shared::HeightTileKey, 225U> nearby_candidates{};
    uint32_t candidate_count = 0U;
    for (int32_t y = -NEAR_RADIUS; y <= NEAR_RADIUS; ++y) {
      for (int32_t x = -NEAR_RADIUS; x <= NEAR_RADIUS; ++x) {
        nearby_candidates[candidate_count++] = {center.x + x, center.y + y};
      }
    }
    std::span<shared::HeightTileKey const> const candidates{nearby_candidates};
    EXPECT_TRUE(queue.contains(center)) << "motion step " << step;
    EXPECT_EQ(queue.nearestPending(candidates), center)
        << "motion step " << step;
    EXPECT_EQ(queue.nearestPending(candidates), center)
        << "unaccepted enqueue consumed pending key";
    EXPECT_TRUE(queue.erase(center)) << "motion step " << step;
    EXPECT_FALSE(queue.contains(center))
        << "accepted enqueue should consume pending key";
  }
}

TEST(PlayerClientCapturePlanTest,
     RepeatedCenterChangesCompleteCurrentNearBeforeBoundedPrewarm) {
    static constexpr uint32_t MAINTENANCE_BUDGET = 256U;
  static constexpr shared::HeightTileKey INITIAL_CENTER{2'048, 2'048};
  static constexpr uint32_t REFRESH_BUDGET = 256U;
  static constexpr uint32_t ARBITRARY_FAR_COUNT = 512U;
  static constexpr uint32_t EXPECTED_PREWARM_OFFSET_COUNT = 904U;
  static constexpr int32_t NEAR_RADIUS =
      client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
  static constexpr std::array<shared::HeightTileKey, 8> MOVED_CENTERS{{
      {1'848, 2'048},
      {2'248, 2'048},
      {1'868, 2'048},
      {2'228, 2'048},
      {1'888, 2'048},
      {2'208, 2'048},
      {1'908, 2'048},
      {2'188, 2'048},
  }};

  EXPECT_EQ(client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS.size(),
            EXPECTED_PREWARM_OFFSET_COUNT);
  for (client::PlayerClientPreviewMeshProbeOffset const offset :
       client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS) {
    int64_t const distance_squared =
        static_cast<int64_t>(offset.x) * offset.x +
        static_cast<int64_t>(offset.y) * offset.y;
    EXPECT_LE(distance_squared,
              static_cast<int64_t>(
                  client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS) *
                  client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS);
    EXPECT_TRUE(offset.x < -NEAR_RADIUS || offset.x > NEAR_RADIUS ||
                offset.y < -NEAR_RADIUS || offset.y > NEAR_RADIUS);
  }

  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> interest;
  auto const add_offsets = [&interest](
                               shared::HeightTileKey const center,
                               auto const &offsets) {
    for (client::PlayerClientPreviewMeshProbeOffset const offset : offsets) {
      interest.insert(shared::normalizeHeightTileKey(
          {center.x + offset.x, center.y + offset.y}));
    }
  };
  for (uint32_t index = 0U; index < ARBITRARY_FAR_COUNT; ++index) {
    interest.insert({INITIAL_CENTER.x + 3'000 + static_cast<int32_t>(index),
                     INITIAL_CENTER.y});
  }
  for (shared::HeightTileKey const center : MOVED_CENTERS) {
    add_offsets(center, client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS);
    add_offsets(center, client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS);
  }

  client::PlayerPreviewMeshQueue queue;
  uint32_t maintenance_budget = MAINTENANCE_BUDGET;
  queue.resetPriority(INITIAL_CENTER, 127, 0, interest);
  for (shared::HeightTileKey const key : interest) {
    ASSERT_TRUE(queue.push(key));
  }

  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> visible;
  for (uint32_t step = 0U; step < static_cast<uint32_t>(MOVED_CENTERS.size());
       ++step) {
    shared::HeightTileKey const center = MOVED_CENTERS[step];
    queue.beginPriorityRefresh(center, step % 2U == 0U ? -127 : 127, 0);
    EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET)
        << "motion step " << step;

    std::array<shared::HeightTileKey,
               client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT>
        nearby_candidates{};
    uint32_t nearby_candidate_count = 0U;
    for (client::PlayerClientPreviewMeshProbeOffset const offset :
         client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
      shared::HeightTileKey const key = shared::normalizeHeightTileKey(
          {center.x + offset.x, center.y + offset.y});
      if (!visible.contains(key)) {
        if (!queue.contains(key)) {
          ASSERT_TRUE(queue.push(key)) << "motion step " << step;
        }
        nearby_candidates[nearby_candidate_count++] = key;
      }
    }
    std::span<shared::HeightTileKey const> const candidates{
        nearby_candidates.data(), nearby_candidate_count};
    std::array<shared::HeightTileKey,
               client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSET_COUNT>
        prewarm_candidates{};
    uint32_t prewarm_candidate_count = 0U;
    uint32_t expected_prewarm_admissions = 0U;
    for (client::PlayerClientPreviewMeshProbeOffset const offset :
         client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS) {
      shared::HeightTileKey const key = shared::normalizeHeightTileKey(
          {center.x + offset.x, center.y + offset.y});
      if (!visible.contains(key) && !queue.contains(key)) {
        ASSERT_TRUE(queue.push(key)) << "motion step " << step;
      }
      prewarm_candidates[prewarm_candidate_count++] = key;
      if (queue.contains(key)) {
        ++expected_prewarm_admissions;
      }
    }
    std::span<shared::HeightTileKey const> const prewarm_span{
        prewarm_candidates.data(), prewarm_candidate_count};
    uint32_t near_admissions = 0U;
    uint32_t prewarm_admissions = 0U;
    bool far_candidate_observed = false;
    int64_t previous_prewarm_distance_squared = -1;
    while (std::optional<client::PlayerPreviewMeshAdmissionCandidate> const
               admission = client::nextPlayerPreviewMeshAdmissionCandidate(
                   queue,
                   maintenance_budget,
                   candidates,
                   true,
                   prewarm_span,
                   true)) {
      if (admission->current_near) {
        ASSERT_EQ(prewarm_admissions, 0U) << "motion step " << step;
        EXPECT_TRUE(std::find(candidates.begin(), candidates.end(),
                              admission->key) != candidates.end())
            << "motion step " << step;
        ++near_admissions;
      } else {
        if (std::find(prewarm_span.begin(), prewarm_span.end(), admission->key) == prewarm_span.end()) {
          EXPECT_EQ(near_admissions, candidates.size()) << "motion step " << step;
          EXPECT_EQ(prewarm_admissions, expected_prewarm_admissions) << "motion step " << step;
          EXPECT_TRUE(interest.contains(admission->key));
          far_candidate_observed = true;
          break;
        }
        EXPECT_TRUE(std::find(prewarm_span.begin(), prewarm_span.end(),
                              admission->key) != prewarm_span.end())
            << "motion step " << step;
        int32_t const x = admission->key.x - center.x;
        int32_t const y = admission->key.y - center.y;
        int64_t const distance_squared =
            static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
        EXPECT_GE(distance_squared, previous_prewarm_distance_squared)
            << "motion step " << step;
        previous_prewarm_distance_squared = distance_squared;
        ++prewarm_admissions;
      }
      ASSERT_TRUE(queue.erase(admission->key)) << "motion step " << step;
      visible.insert(admission->key);
    }

    EXPECT_EQ(near_admissions, candidates.size()) << "motion step " << step;
    EXPECT_EQ(prewarm_admissions, expected_prewarm_admissions)
        << "motion step " << step;
    for (client::PlayerClientPreviewMeshProbeOffset const offset :
         client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
      shared::HeightTileKey const key = shared::normalizeHeightTileKey(
          {center.x + offset.x, center.y + offset.y});
      EXPECT_TRUE(visible.contains(key)) << "motion step " << step;
    }
    EXPECT_GT(queue.size(), REFRESH_BUDGET) << "motion step " << step;
    if (far_candidate_observed) {
      EXPECT_TRUE(queue.hasReady()) << "motion step " << step;
    }
  }
}

TEST(PlayerClientCapturePlanTest,
     X200PrewarmCoversTheNextNearFootprintAcrossFlooringAndWrap) {
  struct MovementCase final {
    char const* name;
    shared::PlayerPosition start;
    shared::Direction direction;
  };
  std::array<MovementCase, 3U> const cases{{
      {
          .name = "axis",
          .start = {.x = 32'000, .y = 32'000, .z = 800},
          .direction = {
              .x = 127U,
              .y = 0U,
              .accelerated = true,
              .speedup = client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_SPEEDUP,
          },
      },
      {
          .name = "diagonal with nonzero tile phase",
          .start = {
              .x = 32'015,
              .y = 16'015,
              .z = 800,
              .x_subcell = 9'999U,
              .y_subcell = 9'999U,
          },
          .direction = {
              .x = 127U,
              .y = 127U,
              .accelerated = true,
              .speedup = client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_SPEEDUP,
          },
      },
      {
          .name = "axis across wrapped world seam",
          .start = {
              .x = shared::World::FLIGHT_MAX_CELL,
              .y = 16'000,
              .z = 800,
          },
          .direction = {
              .x = 127U,
              .y = 0U,
              .accelerated = true,
              .speedup = client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_SPEEDUP,
          },
      },
  }};
  static constexpr int32_t PREVIOUS_PREWARM_RADIUS = 13;
  static constexpr int32_t TILE_EXTENT =
      (shared::World::FLIGHT_MAX_CELL + 1) /
      static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);

  auto const tileCenter = [](shared::Player const& player) {
    return shared::normalizeHeightTileKey(
        {player.x / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH),
         player.y / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH)});
  };
  auto const wrappedOffset = [](int32_t const target, int32_t const origin) {
    return shared::normalizeHeightTileCoordinate(
               target - origin + TILE_EXTENT / 2, TILE_EXTENT) -
           TILE_EXTENT / 2;
  };
  for (MovementCase const& movement : cases) {
    shared::World world{shared::WorldMode::Flight};
    world.spawnPlayer(1U, '@', movement.start);
    ASSERT_TRUE(world.setPlayerMovementCapabilities(
        1U,
        {.bits = static_cast<uint8_t>(shared::MovementCapability::Flight) |
                 static_cast<uint8_t>(
                     shared::MovementCapability::CollisionBypass)}))
        << movement.name;
    std::optional<shared::Player> const before = world.player(1U);
    ASSERT_TRUE(before.has_value()) << movement.name;
    ASSERT_TRUE(world.movePlayer(1U, movement.direction, shared::TICK))
        << movement.name;
    std::optional<shared::Player> const after = world.player(1U);
    ASSERT_TRUE(after.has_value()) << movement.name;

    shared::HeightTileKey const old_center = tileCenter(before.value());
    shared::HeightTileKey const next_center = tileCenter(after.value());
    bool previous_window_missed_next_near = false;
    for (client::PlayerClientPreviewMeshProbeOffset const offset :
         client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
      shared::HeightTileKey const next_near_key =
          shared::normalizeHeightTileKey(
              {next_center.x + offset.x, next_center.y + offset.y});
      int32_t const relative_x = wrappedOffset(next_near_key.x, old_center.x);
      int32_t const relative_y = wrappedOffset(next_near_key.y, old_center.y);
      bool const protected_by_current_near =
          std::abs(relative_x) <=
              client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS &&
          std::abs(relative_y) <=
              client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
      if (protected_by_current_near) {
        continue;
      }

      auto const prewarm_offset = std::find_if(
          client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS.begin(),
          client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS.end(),
          [relative_x, relative_y](
              client::PlayerClientPreviewMeshProbeOffset const candidate) {
            return candidate.x == relative_x && candidate.y == relative_y;
          });
      EXPECT_NE(prewarm_offset,
                client::PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS.end())
          << movement.name << " uncovered next-near offset (" << relative_x
          << ", " << relative_y << ")";
      int64_t const distance_squared =
          static_cast<int64_t>(relative_x) * relative_x +
          static_cast<int64_t>(relative_y) * relative_y;
      previous_window_missed_next_near =
          previous_window_missed_next_near ||
          distance_squared >
              static_cast<int64_t>(PREVIOUS_PREWARM_RADIUS) *
                  PREVIOUS_PREWARM_RADIUS;
    }
    EXPECT_TRUE(previous_window_missed_next_near)
        << movement.name
        << " should demonstrate why the previous 13-tile window was short";
  }
}

TEST(PlayerClientCapturePlanTest,
     IncompleteNearProbeNeverFallsBackToFarPriorityEntries) {
    static constexpr uint32_t MAINTENANCE_BUDGET = 256U;
  static constexpr shared::HeightTileKey INITIAL_CENTER{1'000, 2'048};
  static constexpr shared::HeightTileKey CURRENT_CENTER{2'000, 2'048};
  static constexpr shared::HeightTileKey PREWARM_KEY{1'992, 2'048};
  static constexpr uint32_t PENDING_COUNT = 300U;
  static constexpr uint32_t REFRESH_BUDGET = 256U;
  client::PlayerPreviewMeshQueue queue;
  uint32_t maintenance_budget = MAINTENANCE_BUDGET;
  std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash>
      interest;
  for (uint32_t index = 0U; index < PENDING_COUNT; ++index) {
    shared::HeightTileKey const key{
        CURRENT_CENTER.x - static_cast<int32_t>(PENDING_COUNT - index - 1U),
        CURRENT_CENTER.y,
    };
    interest.insert(key);
    ASSERT_TRUE(queue.push(key));
  }
  queue.resetPriority(INITIAL_CENTER, 127, 0, interest);
  queue.beginPriorityRefresh(CURRENT_CENTER, 127, 0);

  EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
  EXPECT_TRUE(queue.hasReady());
  std::array<shared::HeightTileKey, 1U> const discovered_nearby{CURRENT_CENTER};
  std::array<shared::HeightTileKey, 1U> const discovered_prewarm{PREWARM_KEY};
  std::span<shared::HeightTileKey const> const no_nearby_candidates{};
  std::optional<client::PlayerPreviewMeshAdmissionCandidate> const
      nearby_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
          queue,
          maintenance_budget,
          discovered_nearby,
          false,
          discovered_prewarm,
          false);
  ASSERT_TRUE(nearby_admission.has_value());
  EXPECT_TRUE(nearby_admission->current_near);
  EXPECT_EQ(nearby_admission->key, CURRENT_CENTER);
  ASSERT_TRUE(queue.erase(CURRENT_CENTER));

  EXPECT_FALSE(client::nextPlayerPreviewMeshAdmissionCandidate(
      queue,
      maintenance_budget,
      no_nearby_candidates,
      false,
      discovered_prewarm,
      false));
  EXPECT_FALSE(client::nextPlayerPreviewMeshAdmissionCandidate(
      queue,
      maintenance_budget,
      no_nearby_candidates,
      true,
      discovered_prewarm,
      false));

  while (!queue.hasReady()) {
    uint32_t const refreshed = queue.refreshPriority(REFRESH_BUDGET, interest);
    ASSERT_GT(refreshed, 0U);
    EXPECT_LE(refreshed, REFRESH_BUDGET);
  }
  EXPECT_FALSE(client::nextPlayerPreviewMeshAdmissionCandidate(
      queue,
      maintenance_budget,
      no_nearby_candidates,
      true,
      discovered_prewarm,
      false));
  std::optional<client::PlayerPreviewMeshAdmissionCandidate> const
      prewarm_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
          queue,
          maintenance_budget,
          no_nearby_candidates,
          true,
          discovered_prewarm,
          true);
  ASSERT_TRUE(prewarm_admission.has_value());
  EXPECT_FALSE(prewarm_admission->current_near);
  EXPECT_EQ(prewarm_admission->key, PREWARM_KEY);
  ASSERT_TRUE(queue.erase(PREWARM_KEY));
  std::optional<client::PlayerPreviewMeshAdmissionCandidate> const
      far_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
          queue,
          maintenance_budget,
          no_nearby_candidates,
          true,
          discovered_prewarm,
          true);
  ASSERT_TRUE(far_admission.has_value());
  EXPECT_FALSE(far_admission->current_near);
  EXPECT_EQ(far_admission->key, queue.top());
}

TEST(PlayerClientCapturePlanTest,
     BoundedPrewarmSelectionUsesWrappedPlayerDistance) {
    static constexpr uint32_t MAINTENANCE_BUDGET = 256U;
  static constexpr shared::HeightTileKey CENTER{4'093, 2'048};
  static constexpr shared::HeightTileKey WRAPPED_NEARER{5, 2'048};
  static constexpr shared::HeightTileKey UNWRAPPED_FARTHER{4'084, 2'048};
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{
      WRAPPED_NEARER, UNWRAPPED_FARTHER};
  client::PlayerPreviewMeshQueue queue;
  uint32_t maintenance_budget = MAINTENANCE_BUDGET;
  queue.resetPriority(CENTER, 127, 0, interest);
  ASSERT_TRUE(queue.push(UNWRAPPED_FARTHER));
  ASSERT_TRUE(queue.push(WRAPPED_NEARER));
  std::array<shared::HeightTileKey, 2U> const prewarm_candidates{
      UNWRAPPED_FARTHER, WRAPPED_NEARER};
  std::span<shared::HeightTileKey const> const no_nearby_candidates{};

  std::optional<client::PlayerPreviewMeshAdmissionCandidate> const admission =
      client::nextPlayerPreviewMeshAdmissionCandidate(
          queue,
          maintenance_budget,
          no_nearby_candidates,
          true,
          prewarm_candidates,
          true);

  ASSERT_TRUE(admission.has_value());
  EXPECT_FALSE(admission->current_near);
  EXPECT_EQ(admission->key, WRAPPED_NEARER);
}

TEST(PlayerClientCapturePlanTest, ForecastAdmissionsPrecedePrewarmAndRespectIncompleteProbes)
{
    static constexpr shared::HeightTileKey CENTER{ 2'048, 2'048 };
    static constexpr shared::HeightTileKey CURRENT{ 2'055, 2'055 };
    static constexpr shared::HeightTileKey FORECAST{ 2'062, 2'048 };
    static constexpr shared::HeightTileKey PREWARM{ 2'040, 2'048 };
    static constexpr shared::HeightTileKey FAR{ 2'080, 2'048 };
    static constexpr uint32_t MAINTENANCE_BUDGET = 4U;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        CURRENT, FORECAST, PREWARM, FAR,
    };
    client::PlayerPreviewMeshQueue queue;
    uint32_t maintenance_budget = MAINTENANCE_BUDGET;
    queue.resetPriority(CENTER, 127, 0, interest);
    for (shared::HeightTileKey const key : interest) {
        ASSERT_TRUE(queue.push(key));
    }
    std::array const current{ CURRENT };
    std::array const forecast{ FORECAST };
    std::array const prewarm{ PREWARM };
    std::span<shared::HeightTileKey const> const empty;
    auto const first = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        current,
        false,
        prewarm,
        false,
        forecast,
        false);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->key, CURRENT);
    EXPECT_TRUE(first->current_near);
    ASSERT_TRUE(queue.erase(CURRENT));
    EXPECT_FALSE(client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        false,
        prewarm,
        true,
        forecast,
        true));
    auto const second = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        true,
        prewarm,
        false,
        forecast,
        false);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->key, FORECAST);
    EXPECT_TRUE(second->forecast_near);
    EXPECT_FALSE(second->current_near);
    ASSERT_TRUE(queue.erase(FORECAST));
    EXPECT_FALSE(client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        true,
        prewarm,
        true,
        forecast,
        false));
    auto const third = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        true,
        prewarm,
        true,
        forecast,
        true);
    ASSERT_TRUE(third);
    EXPECT_EQ(third->key, PREWARM);
    ASSERT_TRUE(queue.erase(PREWARM));
    auto const fourth = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        true,
        empty,
        true,
        forecast,
        true);
    ASSERT_TRUE(fourth);
    EXPECT_EQ(fourth->key, FAR);
}

TEST(PlayerClientCapturePlanTest, ForecastTierCoversActualWrappedX200FootprintBeforeMovement)
{
    static constexpr shared::PlayerPosition START{
        .x = 65'530, .y = 65'534, .z = 900, .x_subcell = 9'999U, .y_subcell = 9'999U,
    };
    static constexpr std::array<shared::Direction, 3U> DIRECTIONS{
        shared::Direction{ .x = 127U, .y = 0U, .accelerated = true, .speedup = 200U },
        shared::Direction{ .x = 127U, .y = 127U, .accelerated = true, .speedup = 200U },
        shared::Direction{ .x = 129U, .y = 129U, .z = 90U, .accelerated = true, .speedup = 200U },
    };
    static constexpr int32_t TILE_SIDE = shared::HEIGHT_TILE_SIDE_LENGTH;
    static constexpr uint32_t RADIUS = 256U;
    for (shared::Direction const direction : DIRECTIONS) {
        shared::World world{ shared::WorldMode::Flight };
        world.spawnPlayer(1U, '@', START);
        ASSERT_TRUE(world.setPlayerMovementCapabilities(1U, { .bits = 3U }));
        auto const forecast = world.forecastFlightPlayerPosition(1U, direction);
        ASSERT_TRUE(forecast);
        shared::HeightTileKey const center{ START.x / TILE_SIDE, START.y / TILE_SIDE };
        shared::HeightTileKey const forecast_center{ forecast->x / TILE_SIDE, forecast->y / TILE_SIDE };
        uint32_t footprint_count = 0U;
        uint32_t forecast_only_count = 0U;
        for (client::PlayerClientPreviewMeshProbeOffset const offset : client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
            shared::HeightTileKey const key = shared::normalizeHeightTileKey({
                forecast_center.x + offset.x, forecast_center.y + offset.y,
            });
            ASSERT_TRUE(shared::heightTileWithinInterest(center, key, RADIUS));
            uint8_t const tier = client::playerClientPreviewMeshPriorityTier(center, forecast_center, key);
            EXPECT_LE(tier, 1U);
            forecast_only_count += tier == 1U ? 1U : 0U;
            ++footprint_count;
        }
        EXPECT_EQ(footprint_count, 225U);
        EXPECT_GT(forecast_only_count, 0U);
        std::optional<shared::Player> const before_move = world.player(1U);
        ASSERT_TRUE(before_move.has_value());
        EXPECT_EQ(before_move->x, START.x);
        EXPECT_EQ(before_move->x_subcell, START.x_subcell);
        ASSERT_TRUE(world.movePlayer(1U, direction));
        std::optional<shared::Player> const after_move = world.player(1U);
        ASSERT_TRUE(after_move.has_value());
        EXPECT_EQ(after_move->x / TILE_SIDE, forecast_center.x);
        EXPECT_EQ(after_move->y / TILE_SIDE, forecast_center.y);
    }
}

TEST(PlayerClientCapturePlanTest,
     RequeuedKeyDoesNotResurfaceFromItsStaleHeapEntry) {
  static constexpr shared::HeightTileKey CENTER{2'048, 2'048};
  static constexpr shared::HeightTileKey KEY{2'049, 2'048};
  static constexpr uint32_t REFRESH_BUDGET = 1U;
  std::unordered_set<shared::HeightTileKey,
                     client::PlayerHeightTileKeyHash> const interest{KEY};
  client::PlayerPreviewMeshQueue queue;
  ASSERT_TRUE(queue.push(KEY));
  queue.beginPriorityRefresh(CENTER, 127, 0);
  ASSERT_TRUE(queue.erase(KEY));
  ASSERT_TRUE(queue.push(KEY));
  ASSERT_TRUE(queue.contains(KEY));
  EXPECT_TRUE(queue.hasReady());
  EXPECT_EQ(queue.top(), KEY);
  EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), REFRESH_BUDGET);
  ASSERT_TRUE(queue.hasReady());
  EXPECT_EQ(queue.top(), KEY);
  queue.pop();
  EXPECT_FALSE(queue.contains(KEY));
  ASSERT_TRUE(queue.empty());

  EXPECT_EQ(queue.refreshPriority(REFRESH_BUDGET, interest), 0U);
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.hasReady());
}

TEST(PlayerClientCapturePlanTest, StaleReadyHeapCleanupIsBoundedAndNearCandidatesBypassIt)
{
    static constexpr uint32_t MAINTENANCE_BUDGET = 256U;
    static constexpr shared::HeightTileKey CENTER{ 2'048, 2'048 };
    static constexpr shared::HeightTileKey NEAR{ 2'049, 2'048 };
    static constexpr shared::HeightTileKey FORECAST{ 2'062, 2'048 };
    static constexpr shared::HeightTileKey FIRST_FAR{ 2'248, 2'048 };
    static constexpr shared::HeightTileKey SECOND_FAR{ 2'249, 2'048 };
    static constexpr uint32_t STALE_ENTRIES = 2'048U;
    static constexpr uint32_t BUDGET = 256U;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        CENTER, NEAR, FORECAST, FIRST_FAR, SECOND_FAR,
    };
    client::PlayerPreviewMeshQueue queue;
    uint32_t maintenance_budget = MAINTENANCE_BUDGET;
    queue.resetPriority(CENTER, 127, 0, interest);
    ASSERT_TRUE(queue.push(CENTER));
    for (uint32_t index = 0U; index < STALE_ENTRIES; ++index) {
        ASSERT_TRUE(queue.push(NEAR));
        ASSERT_TRUE(queue.erase(NEAR));
    }
    ASSERT_TRUE(queue.push(NEAR));
    ASSERT_TRUE(queue.push(FORECAST));
    ASSERT_TRUE(queue.push(SECOND_FAR));
    ASSERT_TRUE(queue.push(FIRST_FAR));
    ASSERT_TRUE(queue.erase(CENTER));
    client::PlayerPreviewMeshQueue const& view = queue;
    EXPECT_FALSE(view.hasReady());
    std::array const near{ NEAR };
    std::array const forecast{ FORECAST };
    std::span<shared::HeightTileKey const> const empty;
    auto const near_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        near,
        true,
        empty,
        true,
        forecast,
        true);
    ASSERT_TRUE(near_admission);
    EXPECT_EQ(near_admission->key, NEAR);
    EXPECT_TRUE(near_admission->current_near);
    ASSERT_TRUE(queue.erase(NEAR));
    auto const forecast_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        true,
        empty,
        true,
        forecast,
        true);
    ASSERT_TRUE(forecast_admission);
    EXPECT_EQ(forecast_admission->key, FORECAST);
    EXPECT_TRUE(forecast_admission->forecast_near);
    ASSERT_TRUE(queue.erase(FORECAST));

    for (uint32_t query = 0U; query < BUDGET; ++query) {
        EXPECT_FALSE(view.hasReady());
    }
    EXPECT_EQ(queue.refreshPriority(0U, interest), 0U);
    EXPECT_FALSE(view.hasReady());
    for (uint32_t batch = 0U; batch < STALE_ENTRIES / BUDGET; ++batch) {
        EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
        EXPECT_FALSE(view.hasReady());
    }
    // The anchor and fresh near/forecast tickets also need explicit maintenance.
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), 4U);
    ASSERT_TRUE(view.hasReady());
    EXPECT_EQ(view.top(), FIRST_FAR);
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), 1U);
    queue.pop();
    ASSERT_TRUE(view.hasReady());
    EXPECT_EQ(view.top(), SECOND_FAR);
    queue.pop();
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(view.hasReady());
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), 0U);
}

TEST(PlayerClientCapturePlanTest, PriorityRefreshSharesItsBudgetWithStaleReadyHeapPops)
{
    static constexpr shared::HeightTileKey CENTER{ 2'048, 2'048 };
    static constexpr shared::HeightTileKey STALE_KEY{ 2'049, 2'048 };
    static constexpr shared::HeightTileKey FIRST_FAR{ 2'248, 2'048 };
    static constexpr shared::HeightTileKey SECOND_FAR{ 2'249, 2'048 };
    static constexpr uint32_t STALE_ENTRIES = 4U;
    static constexpr uint32_t BUDGET = 3U;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        FIRST_FAR, SECOND_FAR,
    };
    client::PlayerPreviewMeshQueue queue;
    queue.resetPriority(CENTER, 127, 0, interest);
    ASSERT_TRUE(queue.push(CENTER));
    for (uint32_t index = 0U; index < STALE_ENTRIES; ++index) {
        ASSERT_TRUE(queue.push(STALE_KEY));
        ASSERT_TRUE(queue.erase(STALE_KEY));
    }
    ASSERT_TRUE(queue.erase(CENTER));
    ASSERT_TRUE(queue.push(FIRST_FAR));
    std::array const arrivals{ SECOND_FAR };
    queue.pushBatch(arrivals);
    EXPECT_FALSE(queue.hasReady());
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    EXPECT_FALSE(queue.hasReady());
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    ASSERT_TRUE(queue.hasReady());
    EXPECT_EQ(queue.top(), FIRST_FAR);
    queue.pop();
    ASSERT_TRUE(queue.hasReady());
    EXPECT_EQ(queue.top(), SECOND_FAR);
    queue.pop();
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest, OnePumpBudgetAdmitsNearForecastPrewarmAndManyFarJobs)
{
    static constexpr uint32_t FAR_COUNT = 32U;
    static constexpr uint32_t PUMP_MAINTENANCE_BUDGET = 256U;
    static constexpr shared::HeightTileKey CENTER{2048, 2048};
    static constexpr shared::HeightTileKey CURRENT{2055, 2055};
    static constexpr shared::HeightTileKey FORECAST{2062, 2048};
    static constexpr shared::HeightTileKey PREWARM{2040, 2048};
    static constexpr shared::HeightTileKey FIRST_FAR{2080, 2048};
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest{
        CURRENT, FORECAST, PREWARM};
    for (uint32_t index = 0U; index < FAR_COUNT; ++index) {
        interest.insert({FIRST_FAR.x + static_cast<int32_t>(index), FIRST_FAR.y});
    }
    queue.resetPriority(CENTER, 127, 0, interest);
    for (auto const key : interest) {
        ASSERT_TRUE(queue.push(key));
    }
    uint32_t maintenance_budget = PUMP_MAINTENANCE_BUDGET;
    maintenance_budget -= queue.refreshPriority(maintenance_budget, interest);
    std::array const near{CURRENT};
    std::array const forecast{FORECAST};
    std::array const prewarm{PREWARM};
    for (uint32_t index = 0U; index < FAR_COUNT + 3U; ++index) {
        auto const admission = client::nextPlayerPreviewMeshAdmissionCandidate(
            queue,
            maintenance_budget,
            near,
            true,
            prewarm,
            true,
            forecast,
            true);
        ASSERT_TRUE(admission) << index;
        shared::HeightTileKey const expected = index == 0U ? CURRENT
            : index == 1U ? FORECAST : index == 2U ? PREWARM
            : shared::HeightTileKey{FIRST_FAR.x + static_cast<int32_t>(index - 3U), FIRST_FAR.y};
        EXPECT_EQ(admission->key, expected);
        ASSERT_TRUE(queue.erase(admission->key));
    }
    EXPECT_TRUE(queue.empty());
    EXPECT_GT(maintenance_budget, 0U);
}

TEST(PlayerClientCapturePlanTest, ExhaustedPumpMaintenanceDefersFarButNotCurrentNear)
{
    static constexpr uint32_t STALE_COUNT = 64U;
    static constexpr uint32_t FAR_COUNT = 16U;
    static constexpr uint32_t PUMP_MAINTENANCE_BUDGET = 8U;
    static constexpr uint32_t MAXIMUM_PUMPS = 32U;
    static constexpr shared::HeightTileKey CENTER{2048, 2048};
    static constexpr shared::HeightTileKey NEAR{2049, 2048};
    static constexpr shared::HeightTileKey FIRST_FAR{2080, 2048};
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest{NEAR};
    queue.resetPriority(CENTER, 127, 0, interest);
    ASSERT_TRUE(queue.push(CENTER));
    for (uint32_t index = 0U; index < STALE_COUNT; ++index) {
        ASSERT_TRUE(queue.push(NEAR));
        ASSERT_TRUE(queue.erase(NEAR));
    }
    ASSERT_TRUE(queue.push(NEAR));
    for (uint32_t index = 0U; index < FAR_COUNT; ++index) {
        shared::HeightTileKey const key{FIRST_FAR.x + static_cast<int32_t>(index), FIRST_FAR.y};
        interest.insert(key);
        ASSERT_TRUE(queue.push(key));
    }
    ASSERT_TRUE(queue.erase(CENTER));
    uint32_t maintenance_budget = PUMP_MAINTENANCE_BUDGET;
    maintenance_budget -= queue.refreshPriority(maintenance_budget, interest);
    ASSERT_EQ(maintenance_budget, 0U);
    std::array const near{NEAR};
    std::span<shared::HeightTileKey const> const empty;
    auto const near_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        near,
        true,
        empty,
        true);
    ASSERT_TRUE(near_admission);
    EXPECT_EQ(near_admission->key, NEAR);
    ASSERT_TRUE(queue.erase(NEAR));
    EXPECT_FALSE(client::nextPlayerPreviewMeshAdmissionCandidate(
        queue,
        maintenance_budget,
        empty,
        true,
        empty,
        true));
    EXPECT_EQ(maintenance_budget, 0U);
    uint32_t admitted_far = 0U;
    for (uint32_t pump = 0U; !queue.empty() && pump < MAXIMUM_PUMPS; ++pump) {
        maintenance_budget = PUMP_MAINTENANCE_BUDGET;
        maintenance_budget -= queue.refreshPriority(maintenance_budget, interest);
        while (auto const admission = client::nextPlayerPreviewMeshAdmissionCandidate(
                   queue,
                   maintenance_budget,
                   empty,
                   true,
                   empty,
                   true)) {
            EXPECT_EQ(admission->key,
                (shared::HeightTileKey{FIRST_FAR.x + static_cast<int32_t>(admitted_far), FIRST_FAR.y}));
            ASSERT_TRUE(queue.erase(admission->key));
            ++admitted_far;
        }
        EXPECT_LE(maintenance_budget, PUMP_MAINTENANCE_BUDGET);
    }
    EXPECT_EQ(admitted_far, FAR_COUNT);
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest, NearbyPendingProbeWrapsAcrossTheWorldEdge) {
  static constexpr shared::HeightTileKey CENTER{4'093, 2'048};
  static constexpr shared::HeightTileKey WRAPPED_EAST{0, 2'048};
  static constexpr int32_t NEAR_RADIUS = 7;
  std::array<shared::HeightTileKey, 225U> candidates{};
  std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash>
      interest;
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
  std::span<shared::HeightTileKey const> const candidate_span{candidates};

  EXPECT_TRUE(interest.contains(WRAPPED_EAST));
  EXPECT_EQ(queue.nearestPending(candidate_span), CENTER);
}

TEST(PlayerClientCapturePlanTest,
     MaintainedCoverageMatchesVisibleInterestThroughPublicationAndReset) {
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
      coverage.meshPublished(visible.insert(key).second,
                             interest.contains(key));
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
    EXPECT_EQ(coverage.complete(static_cast<uint32_t>(interest.size())),
              !interest.empty() && expected_visible == interest.size())
        << step;
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

TEST(PlayerClientCapturePlanTest, MovingCentersServiceFarCohortBeforeTheWholeBacklogIsRanked)
{
    static constexpr uint32_t KEY_COUNT = 45'000U;
    static constexpr uint32_t MAINTENANCE_BUDGET = 256U;
    static constexpr uint32_t ADMISSIONS_PER_PUMP = 128U;
    static constexpr uint32_t MAXIMUM_PUMPS = 1'000U;
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    client::PlayerPreviewMeshQueue queue;
    std::vector<shared::HeightTileKey> keys;
    keys.reserve(KEY_COUNT);
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
        shared::HeightTileKey const key{
            3'000 + static_cast<int32_t>(index % 300U),
            3'000 + static_cast<int32_t>(index / 300U),
        };
        keys.push_back(key);
        interest.insert(key);
    }
    queue.pushBatch(keys);
    interest.insert(CENTER);
    interest.insert({ CENTER.x + 1, CENTER.y });
    ASSERT_TRUE(queue.erase(keys.front()));
    interest.erase(keys.front());
    ASSERT_TRUE(queue.erase(keys.back()));
    ASSERT_TRUE(queue.push(keys.back()));
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> admitted;
    for (uint32_t pump = 0U; !queue.empty() && pump < MAXIMUM_PUMPS; ++pump) {
        shared::HeightTileKey const center{ CENTER.x + static_cast<int32_t>(pump % 2U), CENTER.y };
        int8_t const heading = pump % 2U == 0U ? 127 : -127;
        queue.beginPriorityRefresh(center, heading, 0);
        uint32_t const refreshed = queue.refreshPriority(MAINTENANCE_BUDGET, interest);
        ASSERT_LE(refreshed, MAINTENANCE_BUDGET);
        uint32_t maintenance_budget = MAINTENANCE_BUDGET - refreshed;
        std::array const near{ center };
        shared::HeightTileKey const prewarm_key{ center.x + 20, center.y };
        interest.insert(center);
        interest.insert(prewarm_key);
        std::array const prewarm{ prewarm_key };
        ASSERT_TRUE(queue.push(center));
        ASSERT_TRUE(queue.push(prewarm_key));
        auto const near_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
            queue, maintenance_budget, near, true, prewarm, true);
        ASSERT_TRUE(near_admission);
        EXPECT_EQ(near_admission->key, center);
        EXPECT_TRUE(near_admission->current_near);
        ASSERT_TRUE(queue.erase(center));
        auto const prewarm_admission = client::nextPlayerPreviewMeshAdmissionCandidate(
            queue, maintenance_budget, {}, true, prewarm, true);
        ASSERT_TRUE(prewarm_admission);
        EXPECT_EQ(prewarm_admission->key, prewarm_key);
        EXPECT_FALSE(prewarm_admission->current_near);
        ASSERT_TRUE(queue.erase(prewarm_key));
        uint32_t far_admissions = 0U;
        double previous_priority = -1.0;
        for (; far_admissions < ADMISSIONS_PER_PUMP; ++far_admissions) {
            auto const candidate = client::nextPlayerPreviewMeshAdmissionCandidate(
                queue, maintenance_budget, {}, true, {}, true);
            if (!candidate) {
                break;
            }
            EXPECT_FALSE(candidate->current_near);
            ASSERT_TRUE(interest.contains(candidate->key));
            double const priority = shared::heightTileInterestPriority(center, heading, 0, candidate->key);
            EXPECT_GE(priority, previous_priority);
            previous_priority = priority;
            EXPECT_TRUE(admitted.insert(candidate->key).second);
            ASSERT_TRUE(queue.erase(candidate->key));
        }
        if (pump == 0U) {
            ASSERT_GT(far_admissions, 0U);
            EXPECT_LT(admitted.size(), KEY_COUNT - 1U);
        }
        EXPECT_LE(maintenance_budget, MAINTENANCE_BUDGET - refreshed);
    }
    EXPECT_EQ(admitted.size(), KEY_COUNT - 1U);
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest, StaleReadyHeadsReceiveBoundedMaintenanceBeforeTheUnrankedBacklog)
{
    static constexpr shared::HeightTileKey CENTER{ 2'000, 2'000 };
    static constexpr shared::HeightTileKey STALE{ 2'001, 2'000 };
    static constexpr shared::HeightTileKey FAR{ 2'100, 2'000 };
    static constexpr uint32_t STALE_COUNT = 3U;
    static constexpr uint32_t BUDGET = 2U;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest{ FAR };
    client::PlayerPreviewMeshQueue queue;
    queue.resetPriority(CENTER, 127, 0, interest);
    ASSERT_TRUE(queue.push(CENTER));
    for (uint32_t index = 0U; index < STALE_COUNT; ++index) {
        ASSERT_TRUE(queue.push(STALE));
        ASSERT_TRUE(queue.erase(STALE));
    }
    ASSERT_TRUE(queue.push(FAR));
    ASSERT_TRUE(queue.erase(CENTER));
    std::array const unranked{
        shared::HeightTileKey{ 2'200, 2'000 }, shared::HeightTileKey{ 2'201, 2'000 },
        shared::HeightTileKey{ 2'202, 2'000 }, shared::HeightTileKey{ 2'203, 2'000 },
    };
    interest.insert(unranked.begin(), unranked.end());
    queue.pushBatch(unranked);
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    uint32_t maintenance_budget = 0U;
    EXPECT_FALSE(queue.prepareReady(maintenance_budget));
    EXPECT_EQ(maintenance_budget, 0U);
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    ASSERT_TRUE(queue.prepareReady(maintenance_budget));
    EXPECT_EQ(maintenance_budget, 0U);
    EXPECT_EQ(queue.top(), FAR);
    EXPECT_EQ(queue.size(), unranked.size() + 1U);
}

TEST(PlayerClientCapturePlanTest, UnrankedBacklogRefreshUsesExactBudgetAndFinalPriority)
{
    static constexpr uint32_t KEY_COUNT = 8192U;
    static constexpr uint32_t BUDGET = 256U;
    static constexpr shared::HeightTileKey CENTER{ 2050, 2048 };
    client::PlayerPreviewMeshQueue queue;
    std::vector<shared::HeightTileKey> keys;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
        shared::HeightTileKey const key{
            2000 + static_cast<int32_t>(index % 128U),
            2000 + static_cast<int32_t>(index / 128U),
        };
        keys.push_back(key);
        interest.insert(key);
    }
    queue.pushBatch(keys);
    queue.beginPriorityRefresh(CENTER, 127, 0);
    for (uint32_t processed = 0U; processed < KEY_COUNT; processed += BUDGET) {
        EXPECT_EQ(queue.hasReady(), processed > 0U);
        EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    }
    std::sort(keys.begin(), keys.end(), [](auto const first, auto const second) {
        return std::tuple{ shared::heightTileInterestPriority(CENTER, 127, 0, first), first.y, first.x }
            < std::tuple{ shared::heightTileInterestPriority(CENTER, 127, 0, second), second.y, second.x };
    });
    for (auto const key : keys) {
        ASSERT_TRUE(queue.hasReady());
        EXPECT_EQ(queue.top(), key);
        queue.pop();
    }
    EXPECT_TRUE(queue.empty());
    queue.clear();
}

TEST(PlayerClientCapturePlanTest, QueueChurnKeepsNearBypassAndFreshTicketsAcrossRetirement)
{
    static constexpr uint32_t KEY_COUNT = 2048U;
    static constexpr uint32_t BUDGET = 256U;
    static constexpr shared::HeightTileKey CENTER{ 2048, 2048 };
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
        shared::HeightTileKey const key{ 1024 + static_cast<int32_t>(index), 2048 };
        interest.insert(key);
        ASSERT_TRUE(queue.push(key));
    }
    queue.beginPriorityRefresh(CENTER, 127, 0);
    EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    ASSERT_TRUE(queue.erase(CENTER));
    ASSERT_TRUE(queue.push(CENTER));
    std::array<shared::HeightTileKey, 1> const near{ CENTER };
    EXPECT_EQ(queue.nearestPending(near), CENTER);
    EXPECT_TRUE(queue.hasReady());
    for (uint32_t step = 0U; step < 8U; ++step) {
        queue.beginPriorityRefresh({ CENTER.x + static_cast<int32_t>(step), CENTER.y }, 127, 0);
        EXPECT_LE(queue.refreshPriority(BUDGET, interest), BUDGET);
        EXPECT_EQ(queue.nearestPending(near), CENTER);
    }
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.hasReady());
    EXPECT_FALSE(queue.nearestPending(near).has_value());
    EXPECT_TRUE(queue.push(CENTER));
    EXPECT_TRUE(queue.hasReady());
    EXPECT_EQ(queue.top(), CENTER);
    queue.pop();
    EXPECT_TRUE(queue.empty());
}

TEST(PlayerClientCapturePlanTest, ReservedQueueStorageReusesNodesAndBuffersAcrossChurn)
{
    static constexpr uint32_t KEY_COUNT = 64U;
    static constexpr uint32_t CYCLES = 32U;
    static constexpr uint32_t BUDGET = 8U;
    static constexpr uint64_t MAXIMUM_RETAINED_BYTES =
        uint64_t{64U} * 1024U * 1024U;
    static constexpr shared::HeightTileKey CENTER{100, 100};
    client::PlayerPreviewMeshQueue queue;
    queue.reserveStorage(KEY_COUNT);
    for (uint32_t cycle = 0U; cycle < CYCLES; ++cycle) {
        std::vector<shared::HeightTileKey> keys;
        keys.reserve(KEY_COUNT);
        for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
            keys.push_back({CENTER.x + static_cast<int32_t>(index), CENTER.y + static_cast<int32_t>(cycle)});
        }
        std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
            keys.begin(), keys.end()};
        queue.pushBatch(keys, cycle % 2U == 0U);
        queue.beginPriorityRefresh(CENTER, 127, 0);
        for (uint32_t iteration = 0U; iteration < KEY_COUNT; ++iteration) {
            uint32_t const refreshed = queue.refreshPriority(BUDGET, interest);
            EXPECT_LE(refreshed, BUDGET);
            if (refreshed == 0U) {
                break;
            }
        }
        ASSERT_TRUE(queue.hasReady());
        EXPECT_EQ(queue.size(), KEY_COUNT);
        for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
            ASSERT_TRUE(queue.hasReady());
            EXPECT_TRUE(interest.contains(queue.top()));
            queue.pop();
        }
        EXPECT_TRUE(queue.empty());
        EXPECT_LE(queue.storageStats().retained_bytes, MAXIMUM_RETAINED_BYTES);
    }
    EXPECT_GE(queue.storageStats().buffer_reuses, CYCLES);
    EXPECT_GE(queue.storageStats().ticket_reuses, static_cast<uint64_t>(KEY_COUNT) * (CYCLES - 1U));
    queue.clear();
    EXPECT_EQ(queue.storageStats().retained_bytes, 0U);
    queue.reserveStorage(KEY_COUNT);
    EXPECT_TRUE(queue.push(CENTER));
    EXPECT_EQ(queue.top(), CENTER);
}

TEST(PlayerClientCapturePlanTest, RecycledTicketsPreserveNearBypassAndBudgetedStaleCleanup)
{
    static constexpr uint32_t KEY_COUNT = 64U;
    static constexpr uint32_t CYCLES = 16U;
    static constexpr uint32_t BUDGET = 8U;
    static constexpr shared::HeightTileKey CENTER{100, 100};
    client::PlayerPreviewMeshQueue queue;
    queue.reserveStorage(KEY_COUNT);
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    std::vector<shared::HeightTileKey> previous;
    for (uint32_t cycle = 0U; cycle < CYCLES; ++cycle) {
        for (auto const key : previous) {
            EXPECT_TRUE(queue.erase(key));
        }
        previous.clear();
        interest.clear();
        for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
            shared::HeightTileKey const key{CENTER.x + static_cast<int32_t>(index),
                CENTER.y + static_cast<int32_t>(cycle)};
            previous.push_back(key);
            interest.insert(key);
        }
        queue.pushBatch(previous);
        queue.beginPriorityRefresh(CENTER, 127, 0);
        EXPECT_EQ(queue.size(), KEY_COUNT);
        EXPECT_EQ(queue.nearestPending(std::span<shared::HeightTileKey const>{previous.data(), 1U}), previous.front());
        EXPECT_LE(queue.refreshPriority(BUDGET, interest), BUDGET);
    }
    for (uint32_t iteration = 0U; iteration < KEY_COUNT * CYCLES; ++iteration) {
        uint32_t const refreshed = queue.refreshPriority(BUDGET, interest);
        EXPECT_LE(refreshed, BUDGET);
        if (refreshed == 0U) {
            break;
        }
    }
    ASSERT_TRUE(queue.hasReady());
    EXPECT_EQ(queue.top(), previous.front());
    EXPECT_GT(queue.storageStats().ticket_reuses, 0U);
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.hasReady());
    EXPECT_EQ(queue.storageStats().retained_bytes, 0U);
}

TEST(PlayerClientCapturePlanTest, RepeatedUnrankedBatchDrainsPreserveQueueReuse)
{
    static constexpr uint32_t BATCH_COUNT = 129U;
    static constexpr uint32_t KEYS_PER_BATCH = 8U;
    static constexpr uint32_t CYCLES = 2U;
    static constexpr shared::HeightTileKey CENTER{ 100, 100 };
    client::PlayerPreviewMeshQueue queue;
    for (uint32_t cycle = 0U; cycle < CYCLES; ++cycle) {
        for (uint32_t batch = 0U; batch < BATCH_COUNT; ++batch) {
            std::array<shared::HeightTileKey, KEYS_PER_BATCH> keys;
            for (uint32_t index = 0U; index < KEYS_PER_BATCH; ++index) {
                keys[index] = {
                    CENTER.x + static_cast<int32_t>(batch * KEYS_PER_BATCH + index),
                    CENTER.y + static_cast<int32_t>(cycle),
                };
            }
            queue.pushBatch(keys);
        }
        EXPECT_EQ(queue.size(), BATCH_COUNT * KEYS_PER_BATCH);
        queue.clear();
        EXPECT_TRUE(queue.empty());
        EXPECT_FALSE(queue.hasReady());
        EXPECT_EQ(queue.storageStats().retained_bytes, 0U);
        ASSERT_TRUE(queue.push(CENTER));
        EXPECT_EQ(queue.top(), CENTER);
        queue.pop();
        EXPECT_TRUE(queue.empty());
    }
}

namespace {

void replayContinuousPreviewArrivals(bool const batch_arrivals)
{
    static constexpr uint32_t RADIUS = 128U;
    static constexpr uint32_t CENTER_STEPS = 180U;
    static constexpr uint32_t PUMPS_PER_CENTER = 4U;
    static constexpr uint32_t MAINTENANCE = 256U;
    static constexpr uint32_t ADMISSIONS = 128U;
    static constexpr uint32_t WATCHED_COHORTS = 8U;
    static constexpr int32_t WATCHED_X_OFFSET = 79;
    static constexpr int32_t WATCHED_Y_OFFSET = 100;
    static constexpr shared::HeightTileKey INITIAL_CENTER{ 2000, 2000 };
    client::PlayerPreviewMeshQueue queue;
    auto const initial = shared::makeHeightTileInterest(INITIAL_CENTER, 127, 0, RADIUS);
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest(
        initial.keys.begin(), initial.keys.end());
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> never_published;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> watched_published;
    uint32_t watched_departures = 0U;
    uint32_t first_publications = 0U;
    shared::HeightTileKey previous = INITIAL_CENTER;
    for (uint32_t step = 1U; step <= CENTER_STEPS; ++step) {
        shared::HeightTileKey const center{ INITIAL_CENTER.x + static_cast<int32_t>(step), INITIAL_CENTER.y };
        auto const delta = shared::heightTileInterestDelta(previous, center, RADIUS);
        queue.beginPriorityRefresh(center, 127, 0);
        for (auto const key : delta.removals) {
            if (key.y == INITIAL_CENTER.y + WATCHED_Y_OFFSET
                && key.x > INITIAL_CENTER.x + WATCHED_X_OFFSET
                && key.x <= INITIAL_CENTER.x + WATCHED_X_OFFSET + static_cast<int32_t>(WATCHED_COHORTS)) {
                ++watched_departures;
                EXPECT_TRUE(watched_published.contains(key)) << "Unpublished key left interest: " << key.x;
            }
            interest.erase(key);
            never_published.erase(key);
            static_cast<void>(queue.erase(key));
        }
        for (auto const key : delta.additions) {
            interest.insert(key);
            never_published.insert(key);
            if (!batch_arrivals) {
                ASSERT_TRUE(queue.push(key, true));
            }
        }
        if (batch_arrivals) {
            queue.pushBatch(delta.additions, true);
        }
        previous = center;
        for (uint32_t pump = 0U; pump < PUMPS_PER_CENTER; ++pump) {
            // Actual intake/retry paths single-push these closer, already-published keys.
            for (int32_t y = -8; y < 8; ++y) {
                for (int32_t x = 32; x < 48; ++x) {
                    static_cast<void>(queue.push({ center.x + x, center.y + y }));
                }
            }
            uint32_t remaining = MAINTENANCE;
            uint32_t const refreshed = queue.refreshPriority(remaining, interest);
            ASSERT_LE(refreshed, remaining);
            remaining -= refreshed;
            std::array<shared::HeightTileKey, client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> near{};
            std::array<shared::HeightTileKey, client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> forecast{};
            uint32_t probe_index = 0U;
            for (auto const offset : client::PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
                near[probe_index] = { center.x + offset.x, center.y + offset.y };
                forecast[probe_index] = { center.x + 8 + offset.x, center.y + offset.y };
                ++probe_index;
            }
            std::array const prewarm{ shared::HeightTileKey{ center.x + 20, center.y } };
            static_cast<void>(queue.push(near.front()));
            static_cast<void>(queue.push(forecast.front()));
            static_cast<void>(queue.push(prewarm.front()));
            auto const current = client::nextPlayerPreviewMeshAdmissionCandidate(
                queue, remaining, near, true, prewarm, true, forecast, true);
            ASSERT_TRUE(current);
            ASSERT_EQ(current->key, near.front());
            ASSERT_TRUE(current->current_near);
            ASSERT_TRUE(queue.erase(current->key));
            auto const predicted = client::nextPlayerPreviewMeshAdmissionCandidate(
                queue, remaining, {}, true, prewarm, true, forecast, true);
            ASSERT_TRUE(predicted);
            ASSERT_EQ(predicted->key, forecast.front());
            ASSERT_TRUE(predicted->forecast_near);
            ASSERT_TRUE(queue.erase(predicted->key));
            auto const prewarmed = client::nextPlayerPreviewMeshAdmissionCandidate(
                queue, remaining, {}, true, prewarm, true);
            ASSERT_TRUE(prewarmed);
            ASSERT_EQ(prewarmed->key, prewarm.front());
            ASSERT_TRUE(queue.erase(prewarmed->key));
            for (uint32_t admitted = 3U; admitted < ADMISSIONS; ++admitted) {
                auto const candidate = client::nextPlayerPreviewMeshAdmissionCandidate(
                    queue, remaining, near, true, prewarm, true, forecast, true);
                if (!candidate) {
                    break;
                }
                ASSERT_TRUE(interest.contains(candidate->key));
                if (never_published.erase(candidate->key) != 0U) {
                    ++first_publications;
                    if (candidate->key.y == INITIAL_CENTER.y + WATCHED_Y_OFFSET
                        && candidate->key.x > INITIAL_CENTER.x + WATCHED_X_OFFSET
                        && candidate->key.x <= INITIAL_CENTER.x + WATCHED_X_OFFSET
                            + static_cast<int32_t>(WATCHED_COHORTS)) {
                        watched_published.insert(candidate->key);
                    }
                }
                ASSERT_TRUE(queue.erase(candidate->key));
            }
            ASSERT_LE(remaining, MAINTENANCE - refreshed);
        }
    }
    EXPECT_GT(first_publications, 0U);
    EXPECT_EQ(watched_departures, WATCHED_COHORTS);
    EXPECT_EQ(watched_published.size(), WATCHED_COHORTS);
}

} // namespace

TEST(PlayerClientCapturePlanTest, RankedFirstPublicationsSurviveContinuousReplacementChurnUntilDeparture)
{
    replayContinuousPreviewArrivals(false);
}

TEST(PlayerClientCapturePlanTest, UnrankedFirstPublicationsSurviveMovingRefreshAndReplacementChurn)
{
    replayContinuousPreviewArrivals(true);
}

namespace {

void replayFirstPublicationMaintenance(bool const batch_arrivals)
{
    static constexpr uint32_t REPLACEMENTS = 45'000U;
    static constexpr uint32_t MISSING = 64U;
    static constexpr uint32_t MAINTENANCE = 256U;
    static constexpr uint32_t MOVES = 8U;
    static constexpr shared::HeightTileKey CENTER{ 2000, 2000 };
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> interest;
    std::vector<shared::HeightTileKey> replacements;
    replacements.reserve(REPLACEMENTS);
    for (uint32_t index = 0U; index < REPLACEMENTS; ++index) {
        shared::HeightTileKey const key{
            3000 + static_cast<int32_t>(index % 300U),
            3000 + static_cast<int32_t>(index / 300U),
        };
        replacements.push_back(key);
        interest.insert(key);
    }
    queue.pushBatch(replacements, false);
    std::array<shared::HeightTileKey, MISSING> missing{};
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> pending_first;
    for (uint32_t index = 0U; index < MISSING; ++index) {
        missing[index] = { CENTER.x + 100, CENTER.y + static_cast<int32_t>(index) };
        interest.insert(missing[index]);
        pending_first.insert(missing[index]);
        if (!batch_arrivals) {
            ASSERT_TRUE(queue.push(missing[index], true));
        }
    }
    if (batch_arrivals) {
        queue.pushBatch(missing, true);
    }
    for (uint32_t move = 0U; move < MOVES; ++move) {
        shared::HeightTileKey const center{ CENTER.x + static_cast<int32_t>(move), CENTER.y };
        std::array<shared::HeightTileKey, 16U> arrivals{};
        for (uint32_t index = 0U; index < arrivals.size(); ++index) {
            arrivals[index] = {
                center.x + 110,
                CENTER.y + 100 + static_cast<int32_t>(move * 16U + index),
            };
            interest.insert(arrivals[index]);
            pending_first.insert(arrivals[index]);
            if (!batch_arrivals) {
                ASSERT_TRUE(queue.push(arrivals[index], true));
            }
        }
        if (batch_arrivals) {
            queue.pushBatch(arrivals, true);
        }
        queue.beginPriorityRefresh(center, 127, 0);
        uint32_t remaining = MAINTENANCE;
        uint32_t const refreshed = queue.refreshPriority(remaining, interest);
        ASSERT_LE(refreshed, remaining);
        remaining -= refreshed;
        std::array const near{ center };
        interest.insert(center);
        static_cast<void>(queue.push(center, false));
        auto const near_rebuild = client::nextPlayerPreviewMeshAdmissionCandidate(
            queue, remaining, near, true, {}, true);
        ASSERT_TRUE(near_rebuild);
        EXPECT_EQ(near_rebuild->key, center);
        EXPECT_TRUE(near_rebuild->current_near);
        ASSERT_TRUE(queue.erase(center));
        for (uint32_t admitted = 0U; admitted < MISSING && !pending_first.empty(); ++admitted) {
            auto const candidate = client::nextPlayerPreviewMeshAdmissionCandidate(
                queue, remaining, {}, true, {}, true);
            ASSERT_TRUE(candidate);
            ASSERT_TRUE(pending_first.contains(candidate->key))
                << "Replacement maintenance overtook first publication after center move " << move;
            pending_first.erase(candidate->key);
            ASSERT_TRUE(queue.erase(candidate->key));
        }
        EXPECT_LE(remaining, MAINTENANCE - refreshed);
    }
    EXPECT_TRUE(pending_first.empty());
    EXPECT_GE(queue.size(), REPLACEMENTS);
}

} // namespace

TEST(PlayerClientCapturePlanTest, MovingRefreshServicesRankedMissingBeforeLargeReplacementGenerations)
{
    replayFirstPublicationMaintenance(false);
}

TEST(PlayerClientCapturePlanTest, MovingRefreshServicesBatchMissingBeforeLargeReplacementGenerations)
{
    replayFirstPublicationMaintenance(true);
}

TEST(PlayerClientCapturePlanTest, MissingStaleHeadIsPrunedBeforeAReadyReplacementWithinOneSharedBudget)
{
    static constexpr shared::HeightTileKey CENTER{ 2000, 2000 };
    static constexpr shared::HeightTileKey FIRST{ 2001, 2000 };
    static constexpr shared::HeightTileKey STALE{ 2002, 2000 };
    static constexpr shared::HeightTileKey MISSING{ 2003, 2000 };
    static constexpr shared::HeightTileKey REPLACEMENT{ 2004, 2000 };
    client::PlayerPreviewMeshQueue queue;
    queue.beginPriorityRefresh(CENTER, 127, 0);
    ASSERT_TRUE(queue.push(FIRST, true));
    ASSERT_TRUE(queue.push(STALE, true));
    ASSERT_TRUE(queue.push(MISSING, true));
    ASSERT_TRUE(queue.push(REPLACEMENT, false));
    ASSERT_TRUE(queue.erase(STALE));
    ASSERT_TRUE(queue.erase(FIRST));
    EXPECT_TRUE(queue.hasReady());
    uint32_t maintenance = 1U;
    auto const candidate = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue, maintenance, {}, true, {}, true);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate->key, MISSING);
    EXPECT_EQ(maintenance, 0U);
}

TEST(PlayerClientCapturePlanTest, PendingReplacementPromotesToMissingWithoutRevivingItsOldTicket)
{
    static constexpr shared::HeightTileKey CENTER{ 2000, 2000 };
    static constexpr shared::HeightTileKey PROMOTED{ 2100, 2000 };
    static constexpr shared::HeightTileKey REPLACEMENT{ 2040, 2000 };
    static constexpr uint32_t MAINTENANCE = 256U;
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        PROMOTED, REPLACEMENT,
    };
    queue.beginPriorityRefresh(CENTER, 127, 0);
    ASSERT_TRUE(queue.push(PROMOTED, false));
    ASSERT_TRUE(queue.push(REPLACEMENT, false));
    EXPECT_FALSE(queue.push(PROMOTED, true));
    std::array const promoted{ PROMOTED };
    queue.pushBatch(promoted, true);
    EXPECT_EQ(queue.size(), 2U);
    queue.beginPriorityRefresh({ CENTER.x + 1, CENTER.y }, 127, 0);
    uint32_t maintenance = MAINTENANCE;
    maintenance -= queue.refreshPriority(maintenance, interest);
    auto const first = client::nextPlayerPreviewMeshAdmissionCandidate(queue, maintenance, {}, true, {}, true);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->key, PROMOTED);
    ASSERT_TRUE(queue.erase(first->key));
    EXPECT_FALSE(queue.contains(PROMOTED));
    auto const repeat = client::nextPlayerPreviewMeshAdmissionCandidate(queue, maintenance, {}, true, {}, true);
    ASSERT_TRUE(repeat);
    EXPECT_EQ(repeat->key, REPLACEMENT);
    ASSERT_TRUE(queue.erase(repeat->key));
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.prepareReady(maintenance));
    ASSERT_TRUE(queue.push(PROMOTED, false));
    ASSERT_TRUE(queue.push(REPLACEMENT, false));
    queue.pushBatch(promoted, true);
    EXPECT_EQ(queue.size(), 2U);
    auto const batch_promoted = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue, maintenance, {}, true, {}, true);
    ASSERT_TRUE(batch_promoted);
    EXPECT_EQ(batch_promoted->key, PROMOTED);
}

TEST(PlayerClientCapturePlanTest, FirstPublicationPriorityKeepsNearRebuildsAndFreshTicketClassification)
{
    static constexpr shared::HeightTileKey CENTER{ 2000, 2000 };
    static constexpr shared::HeightTileKey NEAR_REBUILD{ 2001, 2000 };
    static constexpr shared::HeightTileKey FORECAST_REBUILD{ 2008, 2000 };
    static constexpr shared::HeightTileKey REBUILD{ 2040, 2000 };
    static constexpr shared::HeightTileKey FIRST{ 2100, 2000 };
    static constexpr shared::HeightTileKey RECYCLED{ 2120, 2000 };
    static constexpr uint32_t MAINTENANCE = 256U;
    client::PlayerPreviewMeshQueue queue;
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{
        NEAR_REBUILD, FORECAST_REBUILD, REBUILD, FIRST, RECYCLED,
    };
    ASSERT_TRUE(queue.push(NEAR_REBUILD, false));
    ASSERT_TRUE(queue.push(FORECAST_REBUILD, false));
    ASSERT_TRUE(queue.push(REBUILD, false));
    ASSERT_TRUE(queue.push(FIRST, true));
    ASSERT_TRUE(queue.push(RECYCLED, true));
    ASSERT_TRUE(queue.erase(RECYCLED));
    ASSERT_TRUE(queue.push(RECYCLED, false));
    queue.beginPriorityRefresh(CENTER, 127, 0);
    uint32_t remaining = MAINTENANCE;
    remaining -= queue.refreshPriority(remaining, interest);
    std::array const near{ NEAR_REBUILD };
    std::array const forecast{ FORECAST_REBUILD };
    auto const current = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue, remaining, near, true, {}, true, forecast, true);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->key, NEAR_REBUILD);
    EXPECT_TRUE(current->current_near);
    ASSERT_TRUE(queue.erase(current->key));
    auto const predicted = client::nextPlayerPreviewMeshAdmissionCandidate(
        queue, remaining, {}, true, {}, true, forecast, true);
    ASSERT_TRUE(predicted);
    EXPECT_EQ(predicted->key, FORECAST_REBUILD);
    EXPECT_TRUE(predicted->forecast_near);
    ASSERT_TRUE(queue.erase(predicted->key));
    auto const first = client::nextPlayerPreviewMeshAdmissionCandidate(queue, remaining, {}, true, {}, true);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->key, FIRST);
    ASSERT_TRUE(queue.erase(first->key));
    auto const replacement = client::nextPlayerPreviewMeshAdmissionCandidate(queue, remaining, {}, true, {}, true);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->key, REBUILD);
}

TEST(PlayerClientCapturePlanTest, MeshEpisodesPreserveCompletedWaitAndPendingCurrentPosition)
{
    client::PlayerMeshEpisodes episodes;
    auto const start = client::PlayerMeshEpisodes::Clock::time_point{};
    shared::HeightTileKey const key{ 100, 0 };
    episodes.enter(key, start);
    episodes.enter(key, start + std::chrono::milliseconds{ 10 });
    episodes.resident(key, start + std::chrono::milliseconds{ 40 });
    episodes.resident(key, start + std::chrono::milliseconds{ 60 });
    auto pending = episodes.snapshot({ 20, 0 }, start + std::chrono::milliseconds{ 100 });
    EXPECT_EQ(pending.entered_count, 1U);
    EXPECT_EQ(pending.pending_count, 1U);
    EXPECT_EQ(pending.oldest_pending_ns, 100'000'000U);
    EXPECT_EQ(pending.oldest_resident_pending_ns, 60'000'000U);
    EXPECT_EQ(pending.pending_by_current_band[5U], 1U);
    episodes.published(key, start + std::chrono::milliseconds{ 120 });
    episodes.published(key, start + std::chrono::milliseconds{ 130 });
    auto const done = episodes.snapshot({ 20, 0 }, start + std::chrono::seconds{ 1 });
    EXPECT_EQ(done.completed_count, 1U);
    EXPECT_EQ(done.completed_total_ns, 120'000'000U);
    EXPECT_EQ(done.completed_max_ns, 120'000'000U);
    EXPECT_EQ(done.completed_latency_buckets[1U], 1U);
    EXPECT_EQ(done.pending_count, 0U);
    EXPECT_EQ(done.tracked_count, 1U);
}

TEST(PlayerClientCapturePlanTest, CensoredEpisodesDistinguishDataArrivalFromResidentMeshWaitAcrossReentry)
{
    static constexpr shared::HeightTileKey KEY{ 100, 0 };
    client::PlayerMeshEpisodes episodes;
    auto const start = client::PlayerMeshEpisodes::Clock::time_point{};
    episodes.enter(KEY, start);
    episodes.leave(KEY, start + std::chrono::seconds{ 3 });
    episodes.enter(KEY, start + std::chrono::seconds{ 5 });
    episodes.resident(KEY, start + std::chrono::milliseconds{ 7950 });
    episodes.leave(KEY, start + std::chrono::seconds{ 8 });
    episodes.enter(KEY, start + std::chrono::seconds{ 10 });
    episodes.resident(KEY, start + std::chrono::milliseconds{ 10'100 });
    episodes.resident(KEY, start + std::chrono::milliseconds{ 10'200 });
    episodes.leave(KEY, start + std::chrono::seconds{ 12 });
    episodes.enter(KEY, start + std::chrono::seconds{ 13 });
    episodes.resident(KEY, start + std::chrono::milliseconds{ 13'020 });
    episodes.published(KEY, start + std::chrono::milliseconds{ 13'050 });
    episodes.leave(KEY, start + std::chrono::seconds{ 14 });
    auto const summary = episodes.snapshot({}, start + std::chrono::seconds{ 14 });
    EXPECT_EQ(summary.entered_count, 4U);
    EXPECT_EQ(summary.completed_count, 1U);
    EXPECT_EQ(summary.censored_count, 3U);
    EXPECT_EQ(summary.never_resident_censored_count, 1U);
    EXPECT_EQ(summary.resident_censored_count, 2U);
    EXPECT_EQ(summary.resident_censored_max_wait_ns, 1'900'000'000U);
    EXPECT_EQ(summary.never_resident_censored_entry_latency_buckets,
        (std::array<uint64_t, 7U>{ 0U, 0U, 0U, 0U, 0U, 1U, 0U }));
    EXPECT_EQ(summary.resident_censored_wait_latency_buckets,
        (std::array<uint64_t, 7U>{ 1U, 0U, 0U, 0U, 1U, 0U, 0U }));
    EXPECT_EQ(summary.censored_latency_buckets,
        (std::array<uint64_t, 7U>{ 0U, 0U, 0U, 0U, 1U, 2U, 0U }));
    EXPECT_EQ(summary.tracked_count, 0U);
}

TEST(PlayerClientCapturePlanTest, CensoredDeparturePresenceDoesNotConfuseEarlierResidencyWithCurrentData)
{
    static constexpr shared::HeightTileKey KEY{ 100, 0 };
    client::PlayerMeshEpisodes episodes;
    auto const start = client::PlayerMeshEpisodes::Clock::time_point{};
    episodes.enter(KEY, start);
    episodes.resident(KEY, start + std::chrono::milliseconds{ 10 });
    episodes.leave(KEY, start + std::chrono::seconds{ 2 }, false);
    episodes.enter(KEY, start + std::chrono::seconds{ 3 });
    episodes.resident(KEY, start + std::chrono::milliseconds{ 3100 });
    episodes.leave(KEY, start + std::chrono::seconds{ 5 }, true);
    episodes.enter(KEY, start + std::chrono::seconds{ 6 });
    episodes.leaveAll(start + std::chrono::seconds{ 8 });
    episodes.enter(KEY, start + std::chrono::seconds{ 9 });
    episodes.resident(KEY, start + std::chrono::milliseconds{ 9100 });
    episodes.published(KEY, start + std::chrono::milliseconds{ 9200 });
    episodes.leave(KEY, start + std::chrono::seconds{ 10 }, false);
    auto const summary = episodes.snapshot({}, start + std::chrono::seconds{ 10 });
    EXPECT_EQ(summary.censored_count, 3U);
    EXPECT_EQ(summary.resident_censored_count, 2U);
    EXPECT_EQ(summary.never_resident_censored_count, 1U);
    EXPECT_EQ(summary.resident_at_departure_censored_entry_latency_buckets,
        (std::array<uint64_t, 7U>{ 0U, 0U, 0U, 0U, 1U, 0U, 0U }));
    EXPECT_EQ(summary.nonresident_at_departure_censored_entry_latency_buckets,
        (std::array<uint64_t, 7U>{ 0U, 0U, 0U, 0U, 1U, 0U, 0U }));
    EXPECT_EQ(summary.unknown_departure_residency_censored_count, 1U);
}

TEST(PlayerClientCapturePlanTest, MeshEpisodesCensorDeparturesAndBoundOwnershipAcrossReentry)
{
    client::PlayerMeshEpisodes episodes;
    auto const start = client::PlayerMeshEpisodes::Clock::time_point{};
    shared::HeightTileKey const key{ 4095, 0 };
    episodes.enter(key, start);
    episodes.leave(key, start + std::chrono::seconds{ 3 });
    episodes.published(key, start + std::chrono::seconds{ 4 });
    episodes.enter(key, start + std::chrono::seconds{ 5 });
    auto pending = episodes.snapshot({ 0, 0 }, start + std::chrono::seconds{ 6 });
    EXPECT_EQ(pending.entered_count, 2U);
    EXPECT_EQ(pending.censored_count, 1U);
    EXPECT_EQ(pending.censored_max_ns, 3'000'000'000U);
    EXPECT_EQ(pending.censored_latency_buckets[5U], 1U);
    EXPECT_EQ(pending.oldest_pending_ns, 1'000'000'000U);
    EXPECT_EQ(pending.pending_by_current_band[0U], 1U);
    for (int32_t index = 0; index < 1000; ++index) {
        shared::HeightTileKey const other{ index, 1 };
        episodes.enter(other, start + std::chrono::seconds{ 6 });
        episodes.published(other, start + std::chrono::seconds{ 7 });
        episodes.leave(other, start + std::chrono::seconds{ 8 });
    }
    auto const churn = episodes.snapshot({ 0, 0 }, start + std::chrono::seconds{ 8 });
    EXPECT_EQ(churn.tracked_count, 1U);
    EXPECT_EQ(churn.completed_count, 1000U);
    episodes.leaveAll(start + std::chrono::seconds{ 9 });
    auto const cleared = episodes.snapshot({ 0, 0 }, start + std::chrono::seconds{ 9 });
    EXPECT_EQ(cleared.tracked_count, 0U);
    EXPECT_EQ(cleared.censored_count, 2U);
    EXPECT_EQ(cleared.entered_count, cleared.completed_count + cleared.censored_count);
}

TEST(PlayerClientCapturePlanTest, DepartedTicketsAreRemovedAndResetDrainsUnrankedBuffers)
{
    static constexpr uint32_t KEY_COUNT = 1024U;
    static constexpr uint32_t BUDGET = 256U;
    static constexpr shared::HeightTileKey CENTER{ 100, 100 };
    client::PlayerPreviewMeshQueue queue;
    std::vector<shared::HeightTileKey> keys;
    keys.reserve(KEY_COUNT);
    for (uint32_t index = 0U; index < KEY_COUNT; ++index) {
        keys.push_back({ static_cast<int32_t>(index), 100 });
    }
    queue.pushBatch(keys);
    std::unordered_set<shared::HeightTileKey, client::PlayerHeightTileKeyHash> const interest{ CENTER };
    queue.beginPriorityRefresh(CENTER, 0, 127);
    for (uint32_t processed = 0U; processed < KEY_COUNT; processed += BUDGET) {
        EXPECT_EQ(queue.refreshPriority(BUDGET, interest), BUDGET);
    }
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_EQ(queue.top(), CENTER);
    queue.beginPriorityRefresh(CENTER, 127, 0);
    queue.resetPriority(CENTER, 0, 127, interest);
    EXPECT_TRUE(queue.hasReady());
    EXPECT_EQ(queue.top(), CENTER);
    queue.clear();
    EXPECT_TRUE(queue.empty());
}
