#include <client/PlayerClient.hpp>

#include <client/Camera.hpp>
#include <client/CameraController.hpp>
#include <client/CameraObstruction.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewMeshing.hpp>
#include <client/PreviewMeshWorkQueue.hpp>
#include <client/render/HeightTileInstanceReaper.hpp>
#include <client/render/TextRenderer.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/Log.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace client {

std::optional<PlayerClientCapturePreset> parsePlayerClientCapturePreset(std::string_view const value) noexcept
{
    if (value == "central-spike") {
        return PlayerClientCapturePreset::CentralSpike;
    }
    if (value == "trench-first-spike") {
        return PlayerClientCapturePreset::TrenchFirstSpike;
    }
    if (value == "mountain-climb") {
        return PlayerClientCapturePreset::MountainClimb;
    }
    if (value == "first-person-origin") {
        return PlayerClientCapturePreset::FirstPersonOrigin;
    }
    return std::nullopt;
}

PlayerClientCapturePlan playerClientCapturePlan(PlayerClientCapturePreset const preset) noexcept
{
    switch (preset) {
    case PlayerClientCapturePreset::CentralSpike:
        return { preset, "central-spike", "S7 CAPTURE: FLYING THIRD PERSON / CENTRAL SPIKE",
            CameraPerspective::ThirdPersonRear, { .yaw_degrees = 110.0, .pitch_degrees = 18.0 },
            { .x = 32'598, .y = 32'768, .z = 420 }, std::nullopt, 105.0, 12.0 };
    case PlayerClientCapturePreset::TrenchFirstSpike:
        return { preset, "trench-first-spike", "S7 CAPTURE: TRENCH / FIRST SPIKE",
            CameraPerspective::ThirdPersonRear, { .yaw_degrees = -90.0, .pitch_degrees = 10.0 },
            { .x = 32'864, .y = 32'768, .z = 300 }, std::nullopt };
    case PlayerClientCapturePreset::MountainClimb:
        return { preset, "mountain-climb", "S7 CAPTURE: MOUNTAIN CLIMB",
            CameraPerspective::ThirdPersonRear, { .yaw_degrees = 75.0, .pitch_degrees = 55.0 },
            { .x = 32'880, .y = 32'768, .z = 347 }, shared::MovementCapabilities{ .bits = 0U } };
    case PlayerClientCapturePreset::FirstPersonOrigin:
        return { preset, "first-person-origin", "S7 CAPTURE: FIRST PERSON / HORIZONTAL X/Z (0, 0)",
            CameraPerspective::FirstPerson, { .yaw_degrees = 0.0, .pitch_degrees = -18.0 },
            { .x = 0, .y = 0, .z = 8 }, std::nullopt };
    }
    return playerClientCapturePlan(PlayerClientCapturePreset::CentralSpike);
}

std::array<PlayerClientCapturePreset, 4> playerClientCapturePresets() noexcept
{
    return { PlayerClientCapturePreset::CentralSpike, PlayerClientCapturePreset::TrenchFirstSpike,
        PlayerClientCapturePreset::MountainClimb, PlayerClientCapturePreset::FirstPersonOrigin };
}

std::chrono::milliseconds playerClientRendererFrameBudget(bool const capture_enabled) noexcept
{
    return capture_enabled ? std::chrono::seconds{1} : std::chrono::milliseconds{8};
}

std::chrono::steady_clock::time_point playerClientPreviewMeshWorkDeadline(
    std::chrono::steady_clock::time_point const renderer_deadline,
    std::chrono::steady_clock::time_point const mesh_work_started_at) noexcept
{
    static constexpr std::chrono::milliseconds FRESH_MESH_WORK_RESERVE{ 1 };
    return std::max(renderer_deadline, mesh_work_started_at + FRESH_MESH_WORK_RESERVE);
}

uint32_t playerClientHeightTileChangeBudget(bool const) noexcept
{
    return 128U;
}

uint32_t playerClientPreviewMeshJobBudget(bool const) noexcept
{
    return 16U;
}

uint32_t playerClientPreviewMeshBacklogCapacity(bool const capture_enabled) noexcept
{
    return capture_enabled ? 16U : 128U;
}

uint32_t playerClientPreviewMeshAdmissionBudget(bool const capture_enabled) noexcept
{
    return capture_enabled ? 16U : 128U;
}

bool PlayerPreviewMeshAdmissionWindow::tryAdmit() noexcept
{
    if (!canAccept()) {
        return false;
    }
    ++m_outstanding;
    return true;
}

bool PlayerPreviewMeshAdmissionWindow::releaseOne() noexcept
{
    if (m_outstanding == 0U) {
        return false;
    }
    --m_outstanding;
    return true;
}

bool playerClientShouldQueuePreviewRemoval(bool const capture_enabled, bool const mesh_visible) noexcept
{
    return !capture_enabled || mesh_visible;
}

uint32_t playerClientPreviewRemovalBudget() noexcept
{
    return 128U;
}

bool playerClientShouldRemovePreviewMesh(bool const currently_interested) noexcept
{
    return !currently_interested;
}

uint32_t playerClientCaptureLogicalSize(
    uint32_t const logical_size,
    uint32_t const framebuffer_size,
    uint32_t const target_framebuffer_size
) noexcept
{
    if (framebuffer_size == 0U || logical_size == 0U) {
        return target_framebuffer_size;
    }
    return std::max(1U, static_cast<uint32_t>(std::lround(
        static_cast<double>(logical_size) * target_framebuffer_size / framebuffer_size
    )));
}

bool playerClientCaptureExtentMeetsMinimum(uint32_t const width, uint32_t const height) noexcept
{
    static constexpr uint32_t MINIMUM_WIDTH = 1'000U;
    static constexpr uint32_t MINIMUM_HEIGHT = 600U;
    return width >= MINIMUM_WIDTH && height >= MINIMUM_HEIGHT;
}

namespace {

static constexpr uint32_t CAPTURE_ROTATION_SECTORS = 16U;
static constexpr int64_t CAPTURE_POSITION_TOLERANCE_SUBCELLS = 500;
static constexpr int64_t FLIGHT_PERIOD_SUBCELLS =
    (static_cast<int64_t>(shared::World::FLIGHT_MAX_CELL) + 1) * shared::SUBCELLS_PER_CELL;
static constexpr uint16_t CAPTURE_NEAR_SPEEDUP = 2U;
static_assert(shared::isFlightSpeedupProfile(CAPTURE_NEAR_SPEEDUP));
constexpr uint32_t CAMERA_OBSTRUCTION_SAMPLE_COUNT = 24U;
constexpr uint32_t CAMERA_OBSTRUCTION_BINARY_STEPS = 8U;
constexpr double CAMERA_OBSTRUCTION_MARGIN = 0.03;
constexpr int32_t HEIGHT_TILE_WORLD_EXTENT = static_cast<int32_t>(
    shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
);

[[noreturn]]
void retirementBatchOverflow(char const* const message) noexcept
{
    try {
        ASSERT(false, "{}", message);
    } catch (...) {
        std::abort();
    }
}

[[nodiscard]] int64_t shortestFlightDelta(int64_t const current, int64_t const target) noexcept
{
    int64_t delta = target - current;
    if (delta > FLIGHT_PERIOD_SUBCELLS / 2) {
        delta -= FLIGHT_PERIOD_SUBCELLS;
    }
    if (delta < -FLIGHT_PERIOD_SUBCELLS / 2) {
        delta += FLIGHT_PERIOD_SUBCELLS;
    }
    return delta;
}

[[nodiscard]] int64_t absoluteValue(int64_t const value) noexcept
{
    return value < 0 ? -value : value;
}

[[nodiscard]] int32_t shortestHeightTileDelta(int32_t const current, int32_t const target) noexcept
{
    int32_t delta = target - current;
    if (delta > HEIGHT_TILE_WORLD_EXTENT / 2) {
        delta -= HEIGHT_TILE_WORLD_EXTENT;
    } else if (delta < -HEIGHT_TILE_WORLD_EXTENT / 2) {
        delta += HEIGHT_TILE_WORLD_EXTENT;
    }
    return delta;
}

struct CurrentNearProbe final {
    HeightTileKey center;
    HeightTileKey key;
};

[[nodiscard]] bool withinCurrentNearProbe(CurrentNearProbe const probe) noexcept
{
    HeightTileKey const normalized_center = shared::normalizeHeightTileKey(probe.center);
    HeightTileKey const normalized_key = shared::normalizeHeightTileKey(probe.key);
    int32_t const dx = shortestHeightTileDelta(normalized_center.x, normalized_key.x);
    int32_t const dy = shortestHeightTileDelta(normalized_center.y, normalized_key.y);
    return dx >= -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS
        && dx <= PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS
        && dy >= -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS
        && dy <= PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
}

[[nodiscard]] bool functionKeyPressed(GLFWwindow* window, int const function_key, int const fallback_key) noexcept
{
    // macOS can expose the top-row keys as media controls unless the user holds
    // Fn.  The number-row aliases keep the controls usable without changing the
    // user's system-wide keyboard preference.
    return glfwGetKey(window, function_key) == GLFW_PRESS
        || glfwGetKey(window, fallback_key) == GLFW_PRESS;
}

[[nodiscard]] HeightTileKey offsetHeightTileKey(
    HeightTileKey const key,
    int32_t const x_offset,
    int32_t const y_offset
) noexcept
{
    return shared::normalizeHeightTileKey({
        .x = key.x + x_offset,
        .y = key.y + y_offset,
    });
}

[[nodiscard]] int32_t floorDivideByHeightTileSide(int32_t const value) noexcept
{
    int32_t result = value / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    if (value < 0 && value % static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) != 0) {
        --result;
    }
    return result;
}

} // namespace

uint16_t playerClientCaptureSpeedup(int64_t const distance_subcells) noexcept
{
    if (distance_subcells > 20'000'000) {
        return 500U;
    }
    if (distance_subcells > 2'000'000) {
        return 200U;
    }
    if (distance_subcells > 200'000) {
        return 30U;
    }
    if (distance_subcells > 40'000) {
        return 5U;
    }
    return CAPTURE_NEAR_SPEEDUP;
}

class PlayerClient::PreviewCacheRetirementReaper final {
private:
    using VisibleMeshSet = std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash>;
    using VisibleTileMap = std::unordered_map<HeightTileKey, HeightTileHandle, PlayerHeightTileKeyHash>;
    using BaseMeshMap = std::unordered_map<HeightTileKey, shared::HeightTileSurfaceMesh, PlayerHeightTileKeyHash>;
    using EdgeProfileMap = std::unordered_map<HeightTileKey, PreviewMeshEdgeProfiles, PlayerHeightTileKeyHash>;
    using SeamBridgeMap = std::unordered_map<HeightTileKey, PreviewMeshSeamBridgeSet, PlayerHeightTileKeyHash>;

    static constexpr uint32_t BATCH_CAPACITY = 128U;
    static constexpr uint64_t NODE_OVERHEAD_BYTES = 64U;

    struct Batch final {
        std::array<VisibleMeshSet::node_type, BATCH_CAPACITY> visible_meshes{};
        std::array<VisibleTileMap::node_type, BATCH_CAPACITY> visible_tiles{};
        std::array<BaseMeshMap::node_type, BATCH_CAPACITY> base_meshes{};
        std::array<EdgeProfileMap::node_type, BATCH_CAPACITY> edge_profiles{};
        std::array<SeamBridgeMap::node_type, BATCH_CAPACITY> seam_bridges{};
        uint32_t visible_mesh_count = 0U;
        uint32_t visible_tile_count = 0U;
        uint32_t base_mesh_count = 0U;
        uint32_t edge_profile_count = 0U;
        uint32_t seam_bridge_count = 0U;
        uint64_t nested_storage_bytes = 0U;

        Batch() = default;
        Batch(Batch const&) = delete;
        Batch& operator=(Batch const&) = delete;
        Batch(Batch&&) noexcept = default;
        Batch& operator=(Batch&&) noexcept = default;

        [[nodiscard]] bool empty() const noexcept
        {
            return visible_mesh_count == 0U && visible_tile_count == 0U && base_mesh_count == 0U
                && edge_profile_count == 0U && seam_bridge_count == 0U;
        }
    };

public:
    void extractRemoved(PlayerClient& client, HeightTileKey const key)
    {
        extractNode(client.m_visible_preview_meshes, key, m_pending.visible_meshes,
            m_pending.visible_mesh_count);

        auto const tile = client.m_visible_preview_mesh_tiles.find(key);
        uint64_t const tile_bytes = tile != client.m_visible_preview_mesh_tiles.end() && tile->second
            ? static_cast<uint64_t>(sizeof(HeightTile))
            : 0U;
        extractNode(client.m_visible_preview_mesh_tiles, key, m_pending.visible_tiles,
            m_pending.visible_tile_count, tile_bytes);

        extractBaseMesh(client, key);
        extractEdgeProfiles(client, key);
        extractSeamBridges(client, key);
    }

    void extractReplacedBase(PlayerClient& client, HeightTileKey const key)
    {
        extractBaseMesh(client, key);
    }

    void extractReplacedTile(PlayerClient& client, HeightTileKey const key)
    {
        auto const tile = client.m_visible_preview_mesh_tiles.find(key);
        uint64_t const tile_bytes = tile != client.m_visible_preview_mesh_tiles.end() && tile->second
            ? static_cast<uint64_t>(sizeof(HeightTile))
            : 0U;
        extractNode(client.m_visible_preview_mesh_tiles, key, m_pending.visible_tiles,
            m_pending.visible_tile_count, tile_bytes);
    }

    void extractReplacedEdgeProfiles(PlayerClient& client, HeightTileKey const key)
    {
        extractEdgeProfiles(client, key);
    }

    void extractReplacedSeamBridges(PlayerClient& client, HeightTileKey const key)
    {
        extractSeamBridges(client, key);
    }

    void extractAll(PlayerClient& client)
    {
        while (!client.m_visible_preview_meshes.empty()) {
            HeightTileKey const key = *client.m_visible_preview_meshes.begin();
            extractRemoved(client, key);
        }
        while (!client.m_visible_preview_mesh_tiles.empty()) {
            HeightTileKey const key = client.m_visible_preview_mesh_tiles.begin()->first;
            extractRemoved(client, key);
        }
        while (!client.m_visible_preview_mesh_bases.empty()) {
            HeightTileKey const key = client.m_visible_preview_mesh_bases.begin()->first;
            extractRemoved(client, key);
        }
        while (!client.m_visible_preview_mesh_edge_profiles.empty()) {
            HeightTileKey const key = client.m_visible_preview_mesh_edge_profiles.begin()->first;
            extractRemoved(client, key);
        }
        while (!client.m_visible_preview_mesh_seam_bridges.empty()) {
            HeightTileKey const key = client.m_visible_preview_mesh_seam_bridges.begin()->first;
            extractRemoved(client, key);
        }
    }

    void submitPending() noexcept
    {
        if (m_pending.empty()) {
            return;
        }
        std::vector<Batch> payload;
        try {
            payload.reserve(1U);
            payload.push_back(std::move(m_pending));
        } catch (...) {
            m_reaper.recordSynchronousFallback();
            m_pending = Batch{};
            return;
        }
        uint64_t const nested_storage_bytes = payload.front().nested_storage_bytes;
        m_pending = Batch{};
        if (m_reaper.retire(payload, nested_storage_bytes)) {
            ++m_submission_count;
        } else {
            payload.clear();
        }
    }

    void drain() noexcept
    {
        submitPending();
        m_reaper.drain();
    }

    [[nodiscard]] uint64_t highWaterBytes() const noexcept { return m_reaper.highWaterBytes(); }
    [[nodiscard]] uint64_t outstandingBytes() const noexcept { return m_reaper.outstandingBytes(); }
    [[nodiscard]] uint64_t synchronousFallbackCount() const noexcept
    {
        return m_reaper.synchronousFallbackCount();
    }
    [[nodiscard]] uint64_t submissionCount() const noexcept { return m_submission_count; }

private:
    template<typename Map, typename NodeArray>
    void extractNode(
        Map& map,
        HeightTileKey const key,
        NodeArray& nodes,
        uint32_t& count,
        uint64_t const nested_storage_bytes = 0U
    )
    {
        if (count == BATCH_CAPACITY) {
            submitPending();
        }
        auto const found = map.find(key);
        if (found == map.end()) {
            return;
        }
        addStorageBytes(m_pending.nested_storage_bytes,
            static_cast<uint64_t>(sizeof(typename Map::value_type)) + NODE_OVERHEAD_BYTES);
        addStorageBytes(m_pending.nested_storage_bytes, nested_storage_bytes);
        if (count >= BATCH_CAPACITY) {
            retirementBatchOverflow("Retired preview cache batch must have an available slot");
        }
        nodes[count] = map.extract(found);
        ++count;
    }

    void extractBaseMesh(PlayerClient& client, HeightTileKey const key)
    {
        auto const base = client.m_visible_preview_mesh_bases.find(key);
        uint64_t const nested_storage_bytes = base == client.m_visible_preview_mesh_bases.end()
            ? 0U
            : capacityBytes(
                static_cast<uint64_t>(base->second.quads.capacity()),
                static_cast<uint64_t>(sizeof(shared::HeightTileSurfaceQuad))
            );
        extractNode(client.m_visible_preview_mesh_bases, key, m_pending.base_meshes,
            m_pending.base_mesh_count, nested_storage_bytes);
    }

    void extractEdgeProfiles(PlayerClient& client, HeightTileKey const key)
    {
        extractNode(client.m_visible_preview_mesh_edge_profiles, key, m_pending.edge_profiles,
            m_pending.edge_profile_count);
    }

    void extractSeamBridges(PlayerClient& client, HeightTileKey const key)
    {
        uint64_t nested_storage_bytes = 0U;
        auto const seams = client.m_visible_preview_mesh_seam_bridges.find(key);
        if (seams != client.m_visible_preview_mesh_seam_bridges.end()) {
            for (std::vector<shared::HeightTileSurfaceQuad> const& edge : seams->second.edges()) {
                addStorageBytes(nested_storage_bytes,
                    capacityBytes(
                        static_cast<uint64_t>(edge.capacity()),
                        static_cast<uint64_t>(sizeof(shared::HeightTileSurfaceQuad))
                    ));
            }
        }
        extractNode(client.m_visible_preview_mesh_seam_bridges, key, m_pending.seam_bridges,
            m_pending.seam_bridge_count, nested_storage_bytes);
    }

    static uint64_t capacityBytes(uint64_t const capacity, uint64_t const value_size) noexcept
    {
        return value_size != 0U && capacity > std::numeric_limits<uint64_t>::max() / value_size
            ? std::numeric_limits<uint64_t>::max()
            : capacity * value_size;
    }

    static void addStorageBytes(uint64_t& total, uint64_t const bytes) noexcept
    {
        if (bytes > std::numeric_limits<uint64_t>::max() - total) {
            total = std::numeric_limits<uint64_t>::max();
        } else {
            total += bytes;
        }
    }

    Batch m_pending;
    HeightTileInstanceReaper<Batch> m_reaper;
    uint64_t m_submission_count = 0U;
};

class PlayerClient::PreviewMeshWorkerPool final {
public:
    enum class Stage : uint8_t {
        WorkerQueued,
        WorkerBuilding,
        CompletedResultWaiting,
    };

    struct StageSummary final {
        uint32_t worker_queued_count = 0U;
        uint32_t worker_building_count = 0U;
        uint32_t completed_result_waiting_count = 0U;
        std::chrono::nanoseconds oldest_worker_queued_age{ 0 };
        std::chrono::nanoseconds oldest_worker_building_age{ 0 };
        std::chrono::nanoseconds oldest_completed_result_waiting_age{ 0 };
    };

    struct StageState final {
        Stage stage;
        std::chrono::steady_clock::time_point started_at;
    };

    struct Job final {
        HeightTileKey key;
        PreviewMeshSource source;
        shared::HeightTileSurfaceDetail target_detail;
        PreviewMeshStage stage;
        uint64_t epoch;
    };

    struct Result final {
        HeightTileKey key;
        PreviewMeshSource source;
        shared::HeightTileSurfaceMesh mesh;
        shared::HeightTileSurfaceDetail target_detail;
        PreviewMeshStage stage;
        uint64_t epoch;
    };

    static constexpr uint32_t RETIREMENT_BATCH_CAPACITY = 256U;

    struct RetirementBatch final {
        std::array<Job, RETIREMENT_BATCH_CAPACITY> jobs{};
        std::array<Result, RETIREMENT_BATCH_CAPACITY> results{};
        uint32_t job_count = 0U;
        uint32_t result_count = 0U;
        uint64_t nested_storage_bytes = 0U;

        RetirementBatch() = default;
        RetirementBatch(RetirementBatch const&) = delete;
        RetirementBatch& operator=(RetirementBatch const&) = delete;
        RetirementBatch(RetirementBatch&&) noexcept = default;
        RetirementBatch& operator=(RetirementBatch&&) noexcept = default;

        [[nodiscard]] bool empty() const noexcept
        {
            return job_count == 0U && result_count == 0U;
        }
    };

    explicit PreviewMeshWorkerPool(uint32_t const maximum_queued_meshes, bool const track_stages)
        : m_admission_window(maximum_queued_meshes), m_track_stages(track_stages)
    {
        if (m_track_stages) {
            m_stage_by_key.reserve(maximum_queued_meshes);
        }
        uint32_t const worker_count = std::clamp(std::thread::hardware_concurrency(), 2U, 8U);
        m_workers.reserve(worker_count);
        for (uint32_t worker = 0U; worker < worker_count; ++worker) {
            m_workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~PreviewMeshWorkerPool()
    {
        {
            std::lock_guard lock{m_mutex};
            m_stopping = true;
        }
        m_work_available.notify_all();
        for (std::thread& worker : m_workers) {
            worker.join();
        }
        submitPendingRetirements();
        {
            std::lock_guard lock{m_mutex};
            for (Job& job : m_work) {
                appendRetiredJob(m_pending_retirements, std::move(job));
            }
            m_work.clear();
            for (Result& result : m_results) {
                appendRetiredResult(m_pending_retirements, std::move(result));
            }
            m_results.clear();
        }
        submitPendingRetirements();
        m_retirement_reaper.drain();
    }

    [[nodiscard]] bool enqueue(Job job)
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_stopping || !m_admission_window.canAccept()) {
                appendRetiredJob(m_pending_retirements, std::move(job));
                return false;
            }
            if (m_track_stages) {
                m_stage_by_key.insert_or_assign(job.key,
                    StageState{
                        .stage = Stage::WorkerQueued,
                        .started_at = std::chrono::steady_clock::now(),
                    });
            }
            m_work.push_back(std::move(job));
            ASSERT(m_admission_window.tryAdmit(), "Preview mesh admission window changed unexpectedly");
        }
        m_work_available.notify_one();
        return true;
    }

    void setPriority(HeightTileKey const center, int8_t const heading_x, int8_t const heading_y,
        std::optional<HeightTileKey> const forecast_center)
    {
        std::lock_guard lock{m_mutex};
        m_priority_center = center;
        m_priority_heading_x = heading_x;
        m_priority_heading_y = heading_y;
        m_priority_forecast_center = forecast_center;
    }

    [[nodiscard]] std::vector<HeightTileKey> cancelQueuedOutsideInterest(
        HeightTileInterestRegion const& interest
    )
    {
        std::vector<HeightTileKey> cancelled;
        submitPendingRetirements();
        {
            std::lock_guard lock{m_mutex};
            cancelled.reserve(m_work.size());
            for (auto job = m_work.begin(); job != m_work.end();) {
                if (interest.contains(job->key)) {
                    ++job;
                    continue;
                }
                cancelled.push_back(job->key);
                appendRetiredJob(m_pending_retirements, std::move(*job));
                if (m_track_stages) {
                    m_stage_by_key.erase(cancelled.back());
                }
                ASSERT(m_admission_window.releaseOne(), "Cancelled preview mesh was not outstanding");
                job = m_work.erase(job);
            }
        }
        submitPendingRetirements();
        return cancelled;
    }

    void discardCompletedResults()
    {
        submitPendingRetirements();
        {
            std::lock_guard lock{m_mutex};
            for (Result& result : m_results) {
                if (m_track_stages) {
                    m_stage_by_key.erase(result.key);
                }
                ASSERT(m_admission_window.releaseOne(), "Discarded preview mesh result was not outstanding");
                appendRetiredResult(m_pending_retirements, std::move(result));
            }
            m_results.clear();
        }
        submitPendingRetirements();
    }

    [[nodiscard]] bool canAccept() const
    {
        std::lock_guard lock{m_mutex};
        return !m_stopping && m_admission_window.canAccept();
    }

    [[nodiscard]] size_t resultCount() const
    {
        std::lock_guard lock{m_mutex};
        return m_results.size();
    }

    [[nodiscard]] size_t outstandingCount() const
    {
        std::lock_guard lock{m_mutex};
        return m_admission_window.outstandingCount();
    }

    [[nodiscard]] StageSummary currentStages(
        std::span<HeightTileKey const> keys, std::chrono::steady_clock::time_point const sampled_at) const
    {
        StageSummary summary;
        if (!m_track_stages) {
            return summary;
        }
        std::lock_guard lock{ m_mutex };
        for (HeightTileKey const key : keys) {
            auto const stage = m_stage_by_key.find(key);
            if (stage == m_stage_by_key.end()) {
                continue;
            }
            std::chrono::nanoseconds const age =
                std::chrono::duration_cast<std::chrono::nanoseconds>(sampled_at - stage->second.started_at);
            switch (stage->second.stage) {
            case Stage::WorkerQueued:
                ++summary.worker_queued_count;
                summary.oldest_worker_queued_age = std::max(summary.oldest_worker_queued_age, age);
                break;
            case Stage::WorkerBuilding:
                ++summary.worker_building_count;
                summary.oldest_worker_building_age = std::max(summary.oldest_worker_building_age, age);
                break;
            case Stage::CompletedResultWaiting:
                ++summary.completed_result_waiting_count;
                summary.oldest_completed_result_waiting_age =
                    std::max(summary.oldest_completed_result_waiting_age, age);
                break;
            }
        }
        return summary;
    }

    [[nodiscard]] std::optional<Result> takeResult()
    {
        std::lock_guard lock{m_mutex};
        std::optional<size_t> const preferred_index = preferredPreviewMeshResultIndex(
            m_results, m_priority_center, m_priority_heading_x, m_priority_heading_y, [this](HeightTileKey const key) {
                return playerClientPreviewMeshPriorityTier(m_priority_center, m_priority_forecast_center, key);
            });
        if (!preferred_index.has_value()) {
            return std::nullopt;
        }
        auto const result_iterator =
            m_results.begin() + static_cast<std::deque<Result>::difference_type>(*preferred_index);
        Result result = std::move(*result_iterator);
        m_results.erase(result_iterator);
        return result;
    }

    void requeueResult(Result result)
    {
        std::lock_guard lock{ m_mutex };
        ASSERT(m_admission_window.outstandingCount() > m_results.size(),
            "Deferred preview mesh result lost its admission");
        m_results.push_back(std::move(result));
    }

    void retireTakenResult(Result&& result)
    {
        appendRetiredResult(m_pending_retirements, std::move(result));
    }

    void drainRetired()
    {
        submitPendingRetirements();
        m_retirement_reaper.drain();
    }

    void submitRetired() noexcept
    {
        submitPendingRetirements();
    }

    void finishResult(HeightTileKey const key)
    {
        std::lock_guard lock{ m_mutex };
        if (m_track_stages) {
            m_stage_by_key.erase(key);
        }
        ASSERT(m_admission_window.releaseOne(), "Preview mesh result was not outstanding");
    }

    [[nodiscard]] std::optional<HeightTileKey> deferLowerPriority(uint8_t const admitted_tier)
    {
        std::lock_guard lock{ m_mutex };
        auto const priority_tier = [this](HeightTileKey const key) {
            return playerClientPreviewMeshPriorityTier(m_priority_center, m_priority_forecast_center, key);
        };
        std::optional<HeightTileKey> key;
        for (uint8_t tier = 3U; tier > admitted_tier && !key; --tier) {
            uint8_t const threshold = static_cast<uint8_t>(tier - 1U);
            auto result = lowestPriorityAbove(m_results, priority_tier, threshold);
            if (result != m_results.end()) {
                key = result->key;
                appendRetiredResult(m_pending_retirements, std::move(*result));
                m_results.erase(result);
            } else {
                auto job = lowestPriorityAbove(m_work, priority_tier, threshold);
                if (job != m_work.end()) {
                    key = job->key;
                    appendRetiredJob(m_pending_retirements, std::move(*job));
                    m_work.erase(job);
                }
            }
        }
        if (!key.has_value()) {
            return std::nullopt;
        }
        if (m_track_stages) {
            m_stage_by_key.erase(*key);
        }
        ASSERT(m_admission_window.releaseOne(), "Deferred preview mesh was not outstanding");
        return key;
    }

private:
    template<typename Range, typename PriorityTier>
    [[nodiscard]] static typename Range::iterator lowestPriorityAbove(
        Range& range,
        PriorityTier const& priority_tier,
        uint8_t const admitted_tier
    )
    {
        auto candidate = range.end();
        uint8_t deferred_tier = admitted_tier;
        for (auto item = range.begin(); item != range.end(); ++item) {
            uint8_t const current_tier = priority_tier(item->key);
            if (current_tier > deferred_tier) {
                candidate = item;
                deferred_tier = current_tier;
            }
        }
        return candidate;
    }

    static void addStorageBytes(uint64_t& total, uint64_t const bytes) noexcept
    {
        if (bytes > std::numeric_limits<uint64_t>::max() - total) {
            total = std::numeric_limits<uint64_t>::max();
        } else {
            total += bytes;
        }
    }

    static uint64_t sourceTileBytes(PreviewMeshSource const& source) noexcept
    {
        uint64_t bytes = source.tile ? static_cast<uint64_t>(sizeof(HeightTile)) : 0U;
        for (HeightTileHandle const& neighbor : source.neighbors) {
            if (neighbor) {
                addStorageBytes(bytes, static_cast<uint64_t>(sizeof(HeightTile)));
            }
        }
        return bytes;
    }

    static uint64_t capacityBytes(uint64_t const capacity, uint64_t const element_size) noexcept
    {
        return element_size != 0U && capacity > std::numeric_limits<uint64_t>::max() / element_size
            ? std::numeric_limits<uint64_t>::max()
            : capacity * element_size;
    }

    static void appendRetiredJob(RetirementBatch& batch, Job&& job) noexcept
    {
        if (batch.job_count >= RETIREMENT_BATCH_CAPACITY) {
            std::abort();
        }
        addStorageBytes(batch.nested_storage_bytes, sourceTileBytes(job.source));
        batch.jobs[batch.job_count++] = std::move(job);
    }

    static void appendRetiredResult(RetirementBatch& batch, Result&& result) noexcept
    {
        if (batch.result_count >= RETIREMENT_BATCH_CAPACITY) {
            std::abort();
        }
        addStorageBytes(batch.nested_storage_bytes, sourceTileBytes(result.source));
        addStorageBytes(batch.nested_storage_bytes,
            capacityBytes(static_cast<uint64_t>(result.mesh.quads.capacity()),
                static_cast<uint64_t>(sizeof(shared::HeightTileSurfaceQuad))));
        batch.results[batch.result_count++] = std::move(result);
    }

    void submitRetirementBatch(RetirementBatch&& batch) noexcept
    {
        if (batch.empty()) {
            return;
        }
        std::vector<RetirementBatch> payload;
        try {
            payload.reserve(1U);
            payload.push_back(std::move(batch));
        } catch (...) {
            m_retirement_reaper.recordSynchronousFallback();
            return;
        }
        uint64_t const nested_storage_bytes = payload.front().nested_storage_bytes;
        if (!m_retirement_reaper.retire(payload, nested_storage_bytes)) {
            payload.clear();
        }
    }

    static void clearRetirementBatch(RetirementBatch& batch) noexcept
    {
        for (uint32_t index = 0U; index < batch.job_count; ++index) {
            batch.jobs[index] = Job{};
        }
        for (uint32_t index = 0U; index < batch.result_count; ++index) {
            batch.results[index] = Result{};
        }
        batch.job_count = 0U;
        batch.result_count = 0U;
        batch.nested_storage_bytes = 0U;
    }

    void submitPendingRetirements() noexcept
    {
        if (m_pending_retirements.empty()) {
            return;
        }
        submitRetirementBatch(std::move(m_pending_retirements));
        clearRetirementBatch(m_pending_retirements);
    }

    void workerLoop()
    {
        auto const priority = [this](HeightTileKey const key) {
            return previewMeshWorkPriority(
                    m_priority_center,
                    m_priority_heading_x,
                    m_priority_heading_y,
                    key,
                [this](HeightTileKey const candidate) {
                    return playerClientPreviewMeshPriorityTier(
                        m_priority_center, m_priority_forecast_center, candidate);
                });
        };
        while (true) {
            Job job{};
            {
                std::unique_lock lock{m_mutex};
                m_work_available.wait(lock, [this] { return m_stopping || !m_work.empty(); });
                if (m_stopping) {
                    return;
                }
                auto const next = std::ranges::min_element(m_work, [&priority](Job const& first, Job const& second) {
                    return priority(first.key) < priority(second.key);
                });
                job = std::move(*next);
                m_work.erase(next);
                if (m_track_stages) {
                    m_stage_by_key.insert_or_assign(job.key,
                        StageState{
                            .stage = Stage::WorkerBuilding,
                            .started_at = std::chrono::steady_clock::now(),
                        });
                }
            }
            shared::HeightTileSurfaceMesh mesh = buildPreviewMesh(job.source);
            Result result{
                .key = job.key,
                .source = std::move(job.source),
                .mesh = std::move(mesh),
                .target_detail = job.target_detail,
                .stage = job.stage,
                .epoch = job.epoch,
            };
            std::lock_guard lock{m_mutex};
            if (m_track_stages) {
                m_stage_by_key.insert_or_assign(job.key,
                    StageState{
                        .stage = Stage::CompletedResultWaiting,
                        .started_at = std::chrono::steady_clock::now(),
                    });
            }
            m_results.push_back(std::move(result));
        }
    }

    PlayerPreviewMeshAdmissionWindow m_admission_window;
    mutable std::mutex m_mutex;
    std::condition_variable m_work_available;
    std::deque<Job> m_work;
    std::deque<Result> m_results;
    std::unordered_map<HeightTileKey, StageState, PlayerHeightTileKeyHash> m_stage_by_key;
    std::vector<std::thread> m_workers;
    bool const m_track_stages;
    RetirementBatch m_pending_retirements;
    HeightTileInstanceReaper<RetirementBatch> m_retirement_reaper;
    HeightTileKey m_priority_center{};
    std::optional<HeightTileKey> m_priority_forecast_center;
    int8_t m_priority_heading_x = 0;
    int8_t m_priority_heading_y = 127;
    bool m_stopping = false;
};

PlayerClient::~PlayerClient()
{
    m_preview_cache_retirement_reaper->extractAll(*this);
    drainPreviewCacheRetirements();
    m_preview_mesh_workers->drainRetired();
    if (m_window.nativeHandle() != nullptr) {
        glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

void PlayerClient::onConnectionStateReset()
{
    m_jump_queued = false;
    ++m_preview_mesh_epoch;
    drainPreviewCacheRetirements();
    m_preview_mesh_workers->drainRetired();
    for (HeightTileKey const key : m_visible_preview_meshes) {
        static_cast<void>(m_renderer.removeHeightTileMesh({ .x = key.x, .y = key.y }));
    }
    m_preview_cache_retirement_reaper->extractAll(*this);
    drainPreviewCacheRetirements();
    m_preview_mesh_coverage.clear();
    m_preview_edge_profile_token = 0U;
    m_preview_lod.clear();
    m_preview_tile_elevations.clear();
    m_preview_forecast_center.reset();
    m_height_tile_interest.clear();
    m_interest_delta_scratch = {};
    std::vector<HeightTileKey>{}.swap(m_remesh_closure_scratch);
    std::vector<HeightTileKey>{}.swap(m_pending_keys_scratch);
    m_interest_storage_radius = 0U;
    m_pending_preview_removals.clear();
    m_pending_preview_removal_set.clear();
    m_pending_preview_meshes.clear();
    m_preview_mesh_pending_ages = {};
    m_preview_mesh_dirty_jobs.clear();
    m_deferred_preview_mesh_result_deadline.reset();
    for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(m_height_tile_interest)) {
        m_preview_mesh_jobs.erase(key);
    }
    m_preview_mesh_workers->discardCompletedResults();
    m_preview_mesh_workers->drainRetired();
    m_preview_mesh_jobs.clear();
    m_interest_heading_x = 0;
    m_interest_heading_y = 127;
    m_applied_interest_heading_x = 0;
    m_applied_interest_heading_y = 0;
    m_capture_started_at.reset();
    m_capture_rotation_started_at.reset();
    m_capture_last_progress_at.reset();
    m_capture_frames = 0U;
    m_capture_height_changes = 0U;
    m_capture_mesh_results = 0U;
    m_capture_mesh_publish_failures = 0U;
    m_capture_mesh_drain_micros = 0U;
    m_capture_mesh_reprioritize_micros = 0U;
    m_capture_last_capability_cycle_source.reset();
    m_capture_rotation_step = 0U;
    m_capture_rotation_started = false;
    m_capture_interest_baseline.clear();
    m_capture_mesh_baseline.clear();
    m_capture_requested = false;
}

void PlayerClient::submitPreviewCacheRetirements() noexcept
{
    m_preview_cache_retirement_reaper->submitPending();
}

void PlayerClient::drainPreviewCacheRetirements() noexcept
{
    m_preview_cache_retirement_reaper->drain();
}

uint64_t PlayerClient::previewCacheRetirementHighWaterBytes() const noexcept
{
    return m_preview_cache_retirement_reaper->highWaterBytes();
}

uint64_t PlayerClient::previewCacheRetirementOutstandingBytes() const noexcept
{
    return m_preview_cache_retirement_reaper->outstandingBytes();
}

uint64_t PlayerClient::previewCacheRetirementFallbackCount() const noexcept
{
    return m_preview_cache_retirement_reaper->synchronousFallbackCount();
}

uint64_t PlayerClient::previewCacheRetirementSubmissionCount() const noexcept
{
    return m_preview_cache_retirement_reaper->submissionCount();
}

PlayerClient::PlayerClient(
    shared::WorldMode const mode,
    std::optional<PlayerClientCaptureOptions> capture,
    std::optional<PlayerClientBenchmarkOptions> benchmark
)
    : GameClient{ mode }
    , m_capture(std::move(capture))
    , m_benchmark(benchmark)
    , m_window(core::platform::glfw::WindowDescriptor{
        .width = 1280U,
        .height = 720U,
        .title = std::string{ shared::PROJECT_NAME },
    })
    , m_renderer(
        VulkanRenderer::createPresentationContext(
            m_window,
            {
                .enable_frame_capture = m_capture.has_value(),
                .require_immediate_present_mode = m_benchmark
                    && m_benchmark->require_immediate_present_mode,
            }
        ),
        m_shader_assets,
        {
            .enable_frame_capture = m_capture.has_value(),
            .require_immediate_present_mode = m_benchmark
                && m_benchmark->require_immediate_present_mode,
        }
    )
    , m_preview_cache_retirement_reaper(std::make_unique<PreviewCacheRetirementReaper>())
    , m_preview_mesh_workers(std::make_unique<PreviewMeshWorkerPool>(
        playerClientPreviewMeshBacklogCapacity(m_capture.has_value()),
              m_benchmark.has_value() && m_benchmark->enable_mesh_stage_diagnostics))
{
    beginContinuousLook();
    static_cast<void>(m_camera.setProjection({
        .far_plane = static_cast<double>(shared::HEIGHT_TILE_INTEREST_RADIUS)
                * shared::HEIGHT_TILE_SIDE_LENGTH
            + shared::WorldExtent::DEPTH,
    }));
    m_renderer.setDebugHudEnabled(true);
    if (m_benchmark) {
        m_renderer.setDebugHudEnabled(m_benchmark->show_debug_hud);
    }
    if (m_capture.has_value()) {
        static constexpr uint32_t CAPTURE_WIDTH = 2560U;
        static constexpr uint32_t CAPTURE_HEIGHT = 1440U;
        int logical_width = 0;
        int logical_height = 0;
        glfwGetWindowSize(m_window.nativeHandle(), &logical_width, &logical_height);
        uint32_t framebuffer_width = 0U;
        uint32_t framebuffer_height = 0U;
        m_window.framebufferSize(framebuffer_width, framebuffer_height);
        if (framebuffer_width != CAPTURE_WIDTH || framebuffer_height != CAPTURE_HEIGHT) {
            glfwSetWindowSize(
                m_window.nativeHandle(),
                static_cast<int>(playerClientCaptureLogicalSize(
                    static_cast<uint32_t>(std::max(0, logical_width)), framebuffer_width, CAPTURE_WIDTH
                )),
                static_cast<int>(playerClientCaptureLogicalSize(
                    static_cast<uint32_t>(std::max(0, logical_height)), framebuffer_height, CAPTURE_HEIGHT
                ))
            );
        }
        PlayerClientCapturePlan const plan = playerClientCapturePlan(m_capture->preset);
        m_camera_perspective = plan.perspective;
        static_cast<void>(m_look_camera.setAngles(plan.look_angles));
        CameraProjection projection = m_camera.projection();
        projection.vertical_fov_degrees = plan.vertical_fov_degrees;
        static_cast<void>(m_camera.setProjection(projection));
    }
}

RendererRuntimeInfo PlayerClient::benchmarkRuntimeInfo() const
{
    return m_renderer.runtimeInfo();
}

void PlayerClient::recordPendingNearMeshAge(shared::HeightTileKey const key,
    std::chrono::steady_clock::time_point const sampled_at,
    PlayerClientBenchmarkTerrainInfo &info) const
{
    auto age = std::ranges::find_if(
        m_preview_mesh_pending_ages, [key](auto const &candidate) { return candidate.active && candidate.key == key; });
    if (age == m_preview_mesh_pending_ages.end()) {
        age =
            std::ranges::find_if(m_preview_mesh_pending_ages, [](auto const &candidate) { return !candidate.active; });
        if (age == m_preview_mesh_pending_ages.end()) {
            return;
        }
        *age = PreviewMeshPendingAge{
            .key = key,
            .started_at = sampled_at,
            .active = true,
        };
    }
    info.oldest_client_pending_near_observed_age = std::max(info.oldest_client_pending_near_observed_age,
        std::chrono::duration_cast<std::chrono::nanoseconds>(sampled_at - age->started_at));
}

PlayerClientBenchmarkTerrainInfo PlayerClient::benchmarkTerrainStreamingInfo() const
{
    PlayerClientBenchmarkTerrainInfo info{
        .center = std::nullopt,
        .accepted_near_admissions = m_benchmark_accepted_near_admissions,
        .accepted_far_admissions = m_benchmark_accepted_far_admissions,
        .far_admissions_while_near_pending = m_benchmark_far_admissions_while_near_pending,
        .mesh_result_drain = {},
    };
    std::optional<HeightTileKey> const center = m_height_tile_interest.center();
    if (!m_benchmark.has_value() || !center.has_value()) {
        return info;
    }

    info.center = center;
    std::array<HeightTileKey, PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> worker_stage_keys{};
    uint32_t worker_stage_key_count = 0U;
    bool const track_mesh_stages = m_benchmark->enable_mesh_stage_diagnostics;
    std::chrono::steady_clock::time_point sampled_at{};
    if (track_mesh_stages) {
        sampled_at = std::chrono::steady_clock::now();
        for (PreviewMeshPendingAge &age : m_preview_mesh_pending_ages) {
            if (age.active) {
                HeightTileHandle const resident = heightTileResidency().resident(age.key);
                auto const visible = m_visible_preview_mesh_tiles.find(age.key);
                bool const mesh_visible = visible != m_visible_preview_mesh_tiles.end() && visible->second == resident;
                if (!withinCurrentNearProbe({ .center = *center, .key = age.key }) ||
                    !m_pending_preview_meshes.contains(age.key) || !resident || mesh_visible) {
                    age.active = false;
                }
            }
        }
    }
    uint32_t index = 0U;
    for (int32_t y = -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
         y <= PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
         ++y) {
        for (int32_t x = -PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
             x <= PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_RADIUS;
             ++x) {
            PlayerClientBenchmarkTerrainTileState& state = info.near_tiles[index++];
            state.key = offsetHeightTileKey(*center, x, y);
            state.desired = m_height_tile_interest.contains(state.key);
            if (!state.desired) {
                continue;
            }
            ++info.desired_count;
            HeightTileHandle const resident = heightTileResidency().resident(state.key);
            state.resident = static_cast<bool>(resident);
            if (!state.resident) {
                ++info.nonresident_count;
                continue;
            }
            ++info.resident_count;
            auto const visible = m_visible_preview_mesh_tiles.find(state.key);
            state.current_mesh_visible = visible != m_visible_preview_mesh_tiles.end()
                && visible->second == resident;
            state.pending = m_pending_preview_meshes.contains(state.key);
            state.active = m_preview_mesh_jobs.contains(state.key);
            if (state.current_mesh_visible) {
                ++info.current_mesh_visible_count;
            }
            if (state.pending) {
                ++info.pending_count;
                if (!state.current_mesh_visible) {
                    ++info.client_pending_admission_count;
                    if (track_mesh_stages) {
                        recordPendingNearMeshAge(state.key, sampled_at, info);
                    }
                }
            }
            if (state.active) {
                ++info.active_count;
            }
            if (!state.current_mesh_visible && !state.pending && !state.active) {
                ++info.unclassified_missing_count;
            }
            if (track_mesh_stages && !state.current_mesh_visible && !state.pending && state.resident) {
                worker_stage_keys[worker_stage_key_count++] = state.key;
            }
        }
    }
    PreviewMeshWorkerPool::StageSummary const worker_stages = track_mesh_stages
        ? m_preview_mesh_workers->currentStages(
              std::span<HeightTileKey const>{ worker_stage_keys.data(), worker_stage_key_count }, sampled_at)
        : PreviewMeshWorkerPool::StageSummary{};
    info.worker_queued_count = worker_stages.worker_queued_count;
    info.worker_building_count = worker_stages.worker_building_count;
    info.completed_result_waiting_count = worker_stages.completed_result_waiting_count;
    info.oldest_worker_queued_age = worker_stages.oldest_worker_queued_age;
    info.oldest_worker_building_age = worker_stages.oldest_worker_building_age;
    info.oldest_completed_result_waiting_age = worker_stages.oldest_completed_result_waiting_age;
    if (track_mesh_stages) {
        info.mesh_result_drain = m_mesh_result_drain_diagnostics;
    }
    return info;
}

PlayerClientBenchmarkFullTerrainInfo PlayerClient::benchmarkFullTerrainStreamingInfo(
    bool const scan_exact_coverage
) const
{
    RendererRuntimeInfo const runtime = m_renderer.runtimeInfo();
    PlayerClientBenchmarkFullTerrainInfo info{
        .center = m_height_tile_interest.center(),
        .exact_current_coverage = std::nullopt,
        .desired_count = m_height_tile_interest.size(),
        .resident_total_count = heightTileResidency().stats().resident_tiles,
        .visible_mesh_count = static_cast<uint32_t>(m_visible_preview_meshes.size()),
        .renderer_mesh_count = runtime.height_tile_mesh_count,
        .renderer_face_count = runtime.chunk_face_count,
        .renderer_draw_face_count = runtime.chunk_draw_face_count,
        .renderer_stone_face_capacity = runtime.stone_face_capacity,
        .pending_delivery_batch_count = static_cast<uint32_t>(m_pending_height_tile_deliveries.size()),
        .pending_residency_change_count = heightTileResidency().pendingChangeCount(),
        .pending_mesh_count = static_cast<uint32_t>(m_pending_preview_meshes.size()),
        .active_mesh_job_count = static_cast<uint32_t>(m_preview_mesh_jobs.size()),
        .dirty_mesh_job_count = static_cast<uint32_t>(m_preview_mesh_dirty_jobs.size()),
        .worker_result_count = static_cast<uint32_t>(m_preview_mesh_workers->resultCount()),
        .worker_outstanding_count = static_cast<uint32_t>(m_preview_mesh_workers->outstandingCount()),
        .pending_removal_count = static_cast<uint32_t>(m_pending_preview_removal_set.size()),
        .resident_desired_count = std::nullopt,
        .nonresident_desired_count = std::nullopt,
        .current_handle_mesh_visible_count = std::nullopt,
        .stale_or_missing_mesh_count = std::nullopt,
        .unclassified_resident_missing_count = std::nullopt,
        .visible_outside_interest_count = std::nullopt,
        .current_visible_mesh_detail_counts = std::nullopt,
        .unclassified_current_visible_mesh_detail_count = std::nullopt,
    };
    uint32_t const expected_count = requiredHeightTileCount();
    info.potentially_ready = !m_height_tile_interest.empty()
        && expected_count != 0U
        && info.desired_count == expected_count
        && m_visible_preview_meshes.size() == expected_count
        && m_visible_preview_mesh_tiles.size() == expected_count
        && info.renderer_mesh_count == expected_count
        && info.pending_delivery_batch_count == 0U
        && info.pending_residency_change_count == 0U
        && info.pending_mesh_count == 0U
        && info.active_mesh_job_count == 0U
        && info.dirty_mesh_job_count == 0U
        && info.worker_result_count == 0U
        && info.worker_outstanding_count == 0U
        && info.pending_removal_count == 0U
        && m_pending_preview_removals.empty();
    if (!scan_exact_coverage) {
        return info;
    }

    PlayerClientBenchmarkTerrainInfo const near_info = benchmarkTerrainStreamingInfo();
    info.client_pending_admission_count = near_info.client_pending_admission_count;
    info.worker_queued_count = near_info.worker_queued_count;
    info.worker_building_count = near_info.worker_building_count;
    info.completed_result_waiting_count = near_info.completed_result_waiting_count;
    info.oldest_client_pending_near_observed_age = near_info.oldest_client_pending_near_observed_age;
    info.oldest_worker_queued_age = near_info.oldest_worker_queued_age;
    info.oldest_worker_building_age = near_info.oldest_worker_building_age;
    info.oldest_completed_result_waiting_age = near_info.oldest_completed_result_waiting_age;

    info.scan_performed = true;
    uint32_t resident_desired_count = 0U;
    uint32_t current_mesh_visible_count = 0U;
    uint32_t stale_or_missing_mesh_count = 0U;
    uint32_t unclassified_resident_missing_count = 0U;
    uint32_t visible_outside_interest_count = 0U;
    std::array<uint32_t, PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_DETAIL_COUNT> current_visible_mesh_detail_counts{};
    uint32_t unclassified_current_visible_mesh_detail_count = 0U;
    static constexpr int32_t WORLD_TILE_COUNT = 4'096;
    HeightTileKey const center = m_height_tile_interest.center().value_or(HeightTileKey{});
    auto const wrapped_distance = [](int32_t const first, int32_t const second) {
        int32_t const direct = std::abs(first - second);
        return std::min(direct, WORLD_TILE_COUNT - direct);
    };
    int32_t const radius = static_cast<int32_t>(m_height_tile_interest.radius());
    for (int32_t y = -radius; y <= radius; ++y) {
        for (int32_t x = -radius; x <= radius; ++x) {
            HeightTileKey const key = offsetHeightTileKey(center, x, y);
            if (!m_height_tile_interest.contains(key)) {
                continue;
            }
            int32_t const distance = std::max(
                wrapped_distance(key.x, center.x),
                wrapped_distance(key.y, center.y)
            );
            size_t const band_index = std::min<size_t>(
                static_cast<size_t>(distance)
                    / static_cast<size_t>(PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_WIDTH),
                PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_COUNT - 1U
            );
            PlayerClientBenchmarkFullTerrainInfo::RadialBand& band = info.radial_bands[band_index];
            ++band.desired_count;
            HeightTileHandle const resident = heightTileResidency().resident(key);
            if (!resident) {
                ++band.nonresident_count;
                continue;
            }
            ++band.resident_count;
            ++resident_desired_count;
            auto const visible = m_visible_preview_mesh_tiles.find(key);
            bool const current_mesh_visible = m_visible_preview_meshes.contains(key)
                && visible != m_visible_preview_mesh_tiles.end()
                && visible->second == resident;
            if (current_mesh_visible) {
                ++band.current_mesh_visible_count;
                ++current_mesh_visible_count;
                auto const visible_base = m_visible_preview_mesh_bases.find(key);
                if (visible_base == m_visible_preview_mesh_bases.end()) {
                    ++unclassified_current_visible_mesh_detail_count;
                } else {
                    switch (visible_base->second.detail) {
                    case shared::HeightTileSurfaceDetail::Coarse:
                        ++current_visible_mesh_detail_counts[0U];
                        break;
                    case shared::HeightTileSurfaceDetail::Coarse2:
                        ++current_visible_mesh_detail_counts[1U];
                        break;
                    case shared::HeightTileSurfaceDetail::Coarse4:
                        ++current_visible_mesh_detail_counts[2U];
                        break;
                    case shared::HeightTileSurfaceDetail::Fine:
                        ++current_visible_mesh_detail_counts[3U];
                        break;
                    case shared::HeightTileSurfaceDetail::Distant:
                        ++current_visible_mesh_detail_counts[4U];
                        break;
                    }
                }
                continue;
            }
            ++band.stale_or_missing_mesh_count;
            ++stale_or_missing_mesh_count;
            if (!m_pending_preview_meshes.contains(key)
                && !m_preview_mesh_jobs.contains(key)
                && !m_preview_mesh_dirty_jobs.contains(key)) {
                ++unclassified_resident_missing_count;
            }
        }
    }
    for (HeightTileKey const key : m_visible_preview_meshes) {
        visible_outside_interest_count += !m_height_tile_interest.contains(key);
    }
    info.resident_desired_count = resident_desired_count;
    info.nonresident_desired_count = info.desired_count - resident_desired_count;
    info.current_handle_mesh_visible_count = current_mesh_visible_count;
    info.stale_or_missing_mesh_count = stale_or_missing_mesh_count;
    info.unclassified_resident_missing_count = unclassified_resident_missing_count;
    info.visible_outside_interest_count = visible_outside_interest_count;
    info.current_visible_mesh_detail_counts = current_visible_mesh_detail_counts;
    info.unclassified_current_visible_mesh_detail_count = unclassified_current_visible_mesh_detail_count;
    info.exact_current_coverage = expected_count != 0U
        && info.desired_count == expected_count
        && resident_desired_count == expected_count
        && current_mesh_visible_count == expected_count
        && stale_or_missing_mesh_count == 0U
        && unclassified_resident_missing_count == 0U
        && visible_outside_interest_count == 0U
        && info.visible_mesh_count == expected_count
        && m_visible_preview_mesh_tiles.size() == expected_count
        && info.renderer_mesh_count == expected_count
        && info.pending_delivery_batch_count == 0U
        && info.pending_residency_change_count == 0U
        && info.pending_mesh_count == 0U
        && info.active_mesh_job_count == 0U
        && info.dirty_mesh_job_count == 0U
        && info.worker_result_count == 0U
        && info.worker_outstanding_count == 0U
        && info.pending_removal_count == 0U
        && m_pending_preview_removals.empty();
    return info;
}

bool PlayerClient::presentationSucceeded() const
{
    return m_last_presentation_succeeded;
}

size_t PlayerHeightTileKeyHash::operator()(shared::HeightTileKey const key) const noexcept
{
    return static_cast<size_t>(shared::heightTileCoordinateHash(key.x, key.y));
}

void PlayerPreviewMeshCoverage::interestAdded(bool const already_visible) noexcept
{
    if (already_visible) {
        ++m_visible_count;
    }
}

void PlayerPreviewMeshCoverage::interestRemoved(bool const visible)
{
    if (visible) {
        ASSERT(m_visible_count > 0U, "Visible interest count cannot underflow");
        --m_visible_count;
    }
}

void PlayerPreviewMeshCoverage::meshPublished(bool const newly_visible, bool const interested) noexcept
{
    if (newly_visible && interested) {
        ++m_visible_count;
    }
}

class PlayerPreviewMeshQueue::RetirementState final {
    static constexpr uint32_t BUFFER_BATCH_SIZE = 16U;
    static constexpr uint32_t NODE_BATCH_SIZE = 256U;
    using Tickets = std::unordered_map<HeightTileKey, uint64_t, PlayerHeightTileKeyHash>;
    struct Batch final {
        std::array<std::vector<Entry>, BUFFER_BATCH_SIZE> buffers;
        std::array<Tickets::node_type, NODE_BATCH_SIZE> nodes;
        uint32_t buffer_count = 0U;
        uint32_t node_count = 0U;
        uint64_t storage_bytes = 0U;
    };

public:
    void reserveStorage(uint32_t const maximum_pending)
    {
        uint64_t const previous_capacity = m_free_nodes.capacity();
        m_node_limit = std::max(previous_capacity,
            std::min(static_cast<uint64_t>(maximum_pending), POOL_BYTES / (2U * NODE_BYTES)));
        uint64_t const additional_bytes = (m_node_limit - previous_capacity) * sizeof(Tickets::node_type);
        while (m_free_buffer_count != 0U && additional_bytes > POOL_BYTES - m_pool_bytes) {
            auto& buffer = m_free_buffers[--m_free_buffer_count];
            m_pool_bytes -= static_cast<uint64_t>(buffer.capacity()) * sizeof(Entry);
            destroyBuffer(buffer);
        }
        m_free_nodes.reserve(m_node_limit);
        m_free_nodes.resize(m_node_limit);
        m_pool_bytes += (static_cast<uint64_t>(m_free_nodes.capacity()) - previous_capacity)
            * sizeof(Tickets::node_type);
    }

    [[nodiscard]] StorageStats storageStats() const noexcept
    {
        return {.retained_bytes = m_pool_bytes, .buffer_reuses = m_buffer_reuses, .ticket_reuses = m_ticket_reuses};
    }

    [[nodiscard]] std::vector<Entry> acquireBuffer(uint64_t const required_entries) noexcept
    {
        uint32_t selected = m_free_buffer_count;
        for (uint32_t index = 0U; index < m_free_buffer_count; ++index) {
            if (m_free_buffers[index].capacity() >= required_entries
                && (selected == m_free_buffer_count
                    || m_free_buffers[index].capacity() < m_free_buffers[selected].capacity())) {
                selected = index;
            }
        }
        if (selected == m_free_buffer_count) {
            return {};
        }
        std::vector<Entry> result = std::move(m_free_buffers[selected]);
        --m_free_buffer_count;
        if (selected != m_free_buffer_count) {
            m_free_buffers[selected] = std::move(m_free_buffers[m_free_buffer_count]);
        }
        m_pool_bytes -= static_cast<uint64_t>(result.capacity()) * sizeof(Entry);
        ++m_buffer_reuses;
        return result;
    }

    void insertTicket(Tickets& tickets, HeightTileKey const key, uint64_t const ticket)
    {
        if (m_free_node_count == 0U) {
            tickets.emplace(key, ticket);
            return;
        }
        Tickets::node_type& node = m_free_nodes[m_free_node_count - 1U];
        node.key() = key;
        node.mapped() = ticket;
        auto inserted = tickets.insert(std::move(node));
        if (!inserted.inserted) {
            node = std::move(inserted.node);
            ASSERT(inserted.inserted, "Recycled preview ticket key must be absent");
            return;
        }
        --m_free_node_count;
        m_pool_bytes -= NODE_BYTES;
        ++m_ticket_reuses;
    }

    void retireBuffer(std::vector<Entry>& buffer) noexcept
    {
        buffer.clear();
        uint64_t const bytes = static_cast<uint64_t>(buffer.capacity()) * sizeof(Entry);
        if (bytes == 0U) {
            return;
        }
        if (m_free_buffer_count < m_free_buffers.size() && bytes <= POOL_BYTES - m_pool_bytes) {
            m_free_buffers[m_free_buffer_count++].swap(buffer);
            m_pool_bytes += bytes;
            return;
        }
        destroyBuffer(buffer);
    }

    void destroyBuffer(std::vector<Entry>& buffer) noexcept
    {
        if (buffer.capacity() == 0U) {
            return;
        }
        uint64_t const bytes = static_cast<uint64_t>(buffer.capacity()) * sizeof(Entry);
        if (bytes > HeightTileInstanceReaper<Batch>::MAXIMUM_BYTES - sizeof(Batch)) {
            m_reaper.recordSynchronousFallback();
            std::vector<Entry>{}.swap(buffer);
            return;
        }
        prepare(bytes);
        if (m_pending.buffer_count == BUFFER_BATCH_SIZE) {
            submit();
        }
        if (m_pending.buffer_count >= BUFFER_BATCH_SIZE) {
            retirementBatchOverflow("Retired buffer batch must have an available slot");
        }
        m_pending.buffers[m_pending.buffer_count++].swap(buffer);
        m_pending.storage_bytes += bytes;
    }

    bool eraseTicket(Tickets& tickets, HeightTileKey const key) noexcept
    {
        auto const found = tickets.find(key);
        if (found == tickets.end()) {
            return false;
        }
        if (m_free_node_count < m_free_nodes.size() && NODE_BYTES <= POOL_BYTES - m_pool_bytes) {
            m_free_nodes[m_free_node_count++] = tickets.extract(found);
            m_pool_bytes += NODE_BYTES;
            return true;
        }
        prepare(NODE_BYTES);
        if (m_pending.node_count == NODE_BATCH_SIZE) {
            submit();
        }
        if (m_pending.node_count >= NODE_BATCH_SIZE) {
            retirementBatchOverflow("Retired ticket batch must have an available slot");
        }
        m_pending.nodes[m_pending.node_count++] = tickets.extract(found);
        m_pending.storage_bytes += NODE_BYTES;
        return true;
    }

    void submit() noexcept
    {
        if (m_pending.buffer_count == 0U && m_pending.node_count == 0U) {
            return;
        }
        std::vector<Batch> payload;
        try {
            payload.reserve(1U);
            payload.push_back(std::move(m_pending));
        } catch (...) {
            m_reaper.recordSynchronousFallback();
            m_pending = Batch{};
            return;
        }
        uint64_t const bytes = payload.front().storage_bytes;
        m_pending = Batch{};
        static_cast<void>(m_reaper.retire(payload, bytes));
    }

    void drain() noexcept
    {
        while (m_free_buffer_count != 0U) {
            destroyBuffer(m_free_buffers[--m_free_buffer_count]);
        }
        for (uint64_t index = 0U; index < m_free_node_count; ++index) {
            prepare(NODE_BYTES);
            if (m_pending.node_count == NODE_BATCH_SIZE) {
                submit();
            }
            if (m_pending.node_count >= NODE_BATCH_SIZE) {
                retirementBatchOverflow("Retired ticket batch must have an available slot");
            }
            m_pending.nodes[m_pending.node_count++] = std::move(m_free_nodes[index]);
            m_pending.storage_bytes += NODE_BYTES;
        }
        std::vector<Tickets::node_type>{}.swap(m_free_nodes);
        m_free_node_count = 0U;
        m_pool_bytes = 0U;
        m_node_limit = 0U;
        submit();
        m_reaper.drain();
    }

private:
    static constexpr uint64_t POOL_BYTES = uint64_t{64U} * 1024U * 1024U;
    static constexpr uint64_t NODE_BYTES = sizeof(Tickets::value_type) + 4U * sizeof(void*);
    void prepare(uint64_t const bytes) noexcept
    {
        static constexpr uint64_t LIMIT = HeightTileInstanceReaper<Batch>::MAXIMUM_BYTES - sizeof(Batch);
        if (bytes > LIMIT || m_pending.storage_bytes > LIMIT - bytes) {
            submit();
        }
    }

    Batch m_pending;
    HeightTileInstanceReaper<Batch> m_reaper;
    std::array<std::vector<Entry>, 32> m_free_buffers;
    std::vector<Tickets::node_type> m_free_nodes;
    uint64_t m_free_node_count = 0U;
    uint32_t m_free_buffer_count = 0U;
    uint64_t m_node_limit = 0U;
    uint64_t m_pool_bytes = 0U;
    uint64_t m_buffer_reuses = 0U;
    uint64_t m_ticket_reuses = 0U;
};

PlayerPreviewMeshQueue::PlayerPreviewMeshQueue()
    : m_retirement(std::make_unique<RetirementState>())
{}

PlayerPreviewMeshQueue::~PlayerPreviewMeshQueue()
{
    clear();
}

void PlayerPreviewMeshQueue::reserveStorage(uint32_t const maximum_pending)
{
    m_retirement->reserveStorage(maximum_pending);
    m_tickets.reserve(maximum_pending);
    ensureHeapCapacity(maximum_pending);
    std::vector<Entry> spare = m_retirement->acquireBuffer(maximum_pending);
    spare.reserve(maximum_pending);
    m_retirement->retireBuffer(spare);
}

PlayerPreviewMeshQueue::StorageStats PlayerPreviewMeshQueue::storageStats() const noexcept
{
    return m_retirement->storageStats();
}

void PlayerPreviewMeshQueue::ensureHeapCapacity(uint64_t const required_entries)
{
    if (required_entries <= m_heap.capacity()) {
        return;
    }
    uint64_t const target_capacity = std::max(required_entries, static_cast<uint64_t>(m_heap.capacity()) * 2U);
    std::vector<Entry> replacement = m_retirement->acquireBuffer(target_capacity);
    replacement.reserve(target_capacity);
    replacement.insert(replacement.end(), m_heap.begin(), m_heap.end());
    m_heap.swap(replacement);
    m_retirement->retireBuffer(replacement);
}

void PlayerPreviewMeshQueue::clear() noexcept
{
    m_retirement->retireBuffer(m_heap);
    for (auto& buffer : m_unranked) {
        m_retirement->retireBuffer(buffer);
    }
    m_unranked.clear();
    while (!m_tickets.empty()) {
        static_cast<void>(m_retirement->eraseTicket(m_tickets, m_tickets.begin()->first));
    }
    decltype(m_tickets){}.swap(m_tickets);
    m_retirement->drain();
}

bool PlayerPreviewMeshQueue::push(HeightTileKey const key)
{
    if (m_tickets.contains(key)) {
        return false;
    }
    ensureHeapCapacity(static_cast<uint64_t>(m_heap.size()) + 1U);
    uint64_t const ticket = m_next_ticket++;
    m_retirement->insertTicket(m_tickets, key, ticket);
    m_heap.push_back(entryFor(key, ticket));
    std::push_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    return true;
}

void PlayerPreviewMeshQueue::pushBatch(std::span<HeightTileKey const> const keys)
{
    if (keys.empty()) {
        return;
    }
    m_unranked.emplace_back();
    std::vector<Entry>& batch = m_unranked.back();
    try {
        batch = m_retirement->acquireBuffer(keys.size());
        batch.reserve(keys.size());
    } catch (...) {
        m_retirement->retireBuffer(batch);
        m_unranked.pop_back();
        throw;
    }
    try {
        for (HeightTileKey const key : keys) {
            if (m_tickets.contains(key)) {
                continue;
            }
            uint64_t const ticket = m_next_ticket++;
            m_retirement->insertTicket(m_tickets, key, ticket);
            batch.push_back({ .priority = 0.0, .key = key, .ticket = ticket });
        }
    } catch (...) {
        if (batch.empty()) {
            m_retirement->retireBuffer(batch);
            m_unranked.pop_back();
        }
        m_retirement->submit();
        throw;
    }
    if (batch.empty()) {
        m_retirement->retireBuffer(batch);
        m_unranked.pop_back();
        m_retirement->submit();
        return;
    }
}

void PlayerPreviewMeshQueue::resetPriority(
    HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& interest
)
{
    uint64_t required_entries = static_cast<uint64_t>(m_heap.size());
    for (auto const& heap : m_unranked) {
        required_entries += static_cast<uint64_t>(heap.size());
    }
    ensureHeapCapacity(required_entries);
    for (auto& heap : m_unranked) {
        m_heap.insert(m_heap.end(), heap.begin(), heap.end());
        m_retirement->retireBuffer(heap);
    }
    m_unranked.clear();
    m_center = center;
    m_heading_x = heading_x;
    m_heading_y = heading_y;
    std::erase_if(m_heap, [this, &interest](Entry const& entry) {
        if (!isCurrent(entry)) {
            return true;
        }
        if (interest.contains(entry.key)) {
            return false;
        }
        static_cast<void>(m_retirement->eraseTicket(m_tickets, entry.key));
        return true;
    });
    for (Entry& entry : m_heap) {
        entry.priority = shared::heightTileInterestPriority(
            m_center, m_heading_x, m_heading_y, entry.key
        );
    }
    std::make_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    m_retirement->submit();
}

void PlayerPreviewMeshQueue::beginPriorityRefresh(
    HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y
)
{
    m_center = center;
    m_heading_x = heading_x;
    m_heading_y = heading_y;
    if (!m_heap.empty()) {
        m_unranked.push_back(std::move(m_heap));
        m_heap = m_retirement->acquireBuffer(0U);
    }
}

uint32_t PlayerPreviewMeshQueue::refreshPriority(
    uint32_t const maximum_entries,
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& interest
)
{
    if (maximum_entries == 0U) {
        m_retirement->submit();
        return 0U;
    }
    uint64_t const maximum_additions = std::min(
        static_cast<uint64_t>(maximum_entries), static_cast<uint64_t>(m_tickets.size())
    );
    if (!m_unranked.empty()) {
        ensureHeapCapacity(static_cast<uint64_t>(m_heap.size()) + maximum_additions);
    }
    uint32_t refreshed = 0U;
    while (refreshed < maximum_entries && !m_unranked.empty()) {
        std::vector<Entry>& heap = m_unranked.front();
        Entry entry = heap.back();
        heap.pop_back();
        if (heap.empty()) {
            m_retirement->retireBuffer(heap);
            m_unranked.pop_front();
        }
        ++refreshed;
        if (!isCurrent(entry)) {
            continue;
        }
        if (!interest.contains(entry.key)) {
            static_cast<void>(m_retirement->eraseTicket(m_tickets, entry.key));
            continue;
        }
        entry = entryFor(entry.key, entry.ticket);
        m_heap.push_back(entry);
        std::push_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    }
    refreshed += pruneReadyHeap(maximum_entries - refreshed);
    m_retirement->submit();
    return refreshed;
}

uint32_t PlayerPreviewMeshQueue::refreshPriority(
    uint32_t const maximum_entries,
    HeightTileInterestRegion const& interest
)
{
    if (maximum_entries == 0U) {
        m_retirement->submit();
        return 0U;
    }
    uint64_t const maximum_additions = std::min(
        static_cast<uint64_t>(maximum_entries), static_cast<uint64_t>(m_tickets.size())
    );
    if (!m_unranked.empty()) {
        ensureHeapCapacity(static_cast<uint64_t>(m_heap.size()) + maximum_additions);
    }
    uint32_t refreshed = 0U;
    while (refreshed < maximum_entries && !m_unranked.empty()) {
        std::vector<Entry>& heap = m_unranked.front();
        Entry entry = heap.back();
        heap.pop_back();
        if (heap.empty()) {
            m_retirement->retireBuffer(heap);
            m_unranked.pop_front();
        }
        ++refreshed;
        if (!isCurrent(entry)) {
            continue;
        }
        if (!interest.contains(entry.key)) {
            static_cast<void>(m_retirement->eraseTicket(m_tickets, entry.key));
            continue;
        }
        entry = entryFor(entry.key, entry.ticket);
        m_heap.push_back(entry);
        std::push_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    }
    refreshed += pruneReadyHeap(maximum_entries - refreshed);
    m_retirement->submit();
    return refreshed;
}

bool PlayerPreviewMeshQueue::contains(HeightTileKey const key) const noexcept
{
    return m_tickets.contains(key);
}

std::optional<HeightTileKey> PlayerPreviewMeshQueue::nearestPending(
    std::span<HeightTileKey const> const candidates
) const noexcept
{
    std::optional<Entry> nearest;
    for (HeightTileKey const key : candidates) {
        auto const ticket = m_tickets.find(key);
        if (ticket == m_tickets.end()) {
            continue;
        }
        Entry const candidate = entryFor(key, ticket->second);
        if (!nearest.has_value() || lowerPriority(*nearest, candidate)) {
            nearest = candidate;
        }
    }
    if (!nearest.has_value()) {
        return std::nullopt;
    }
    return nearest->key;
}

bool PlayerPreviewMeshQueue::empty() const noexcept
{
    return m_tickets.empty();
}

bool PlayerPreviewMeshQueue::hasReady() const noexcept
{
    return m_unranked.empty() && !m_heap.empty() && isCurrent(m_heap.front());
}

bool PlayerPreviewMeshQueue::prepareReady(uint32_t& maintenance_budget) noexcept
{
    if (!m_unranked.empty()) {
        return false;
    }
    if (!hasReady()) {
        maintenance_budget -= pruneReadyHeap(maintenance_budget);
    }
    return hasReady();
}

uint8_t playerClientPreviewMeshPriorityTier(
    HeightTileKey const center,
    std::optional<HeightTileKey> const forecast_center,
    HeightTileKey const key
) noexcept
{
    if (withinCurrentNearProbe({ .center = center, .key = key })) {
        return 0U;
    }
    if (forecast_center && withinCurrentNearProbe({ .center = *forecast_center, .key = key })) {
        return 1U;
    }
    int64_t const dx = shortestHeightTileDelta(center.x, key.x);
    int64_t const dy = shortestHeightTileDelta(center.y, key.y);
    int64_t constexpr PREWARM_RADIUS = PLAYER_CLIENT_PREVIEW_MESH_PREWARM_RADIUS;
    return dx * dx + dy * dy <= PREWARM_RADIUS * PREWARM_RADIUS ? 2U : 3U;
}

std::optional<PlayerPreviewMeshAdmissionCandidate> nextPlayerPreviewMeshAdmissionCandidate(
    PlayerPreviewMeshQueue &pending,
    uint32_t& maintenance_budget,
    std::span<HeightTileKey const> const nearby_candidates,
    bool const nearby_probe_complete,
    std::span<HeightTileKey const> const prewarm_candidates,
    bool const prewarm_probe_complete,
    std::span<HeightTileKey const> const forecast_candidates,
    bool const forecast_probe_complete)
{
    if (std::optional<HeightTileKey> const nearby = pending.nearestPending(nearby_candidates)) {
        return PlayerPreviewMeshAdmissionCandidate{ .key = *nearby, .current_near = true };
    }
    if (!nearby_probe_complete) {
        return std::nullopt;
    }
    if (std::optional<HeightTileKey> const forecast = pending.nearestPending(forecast_candidates)) {
        return PlayerPreviewMeshAdmissionCandidate{ .key = *forecast, .forecast_near = true };
    }
    if (!forecast_probe_complete || !prewarm_probe_complete) {
        return std::nullopt;
    }
    if (std::optional<HeightTileKey> const prewarm = pending.nearestPending(prewarm_candidates)) {
        return PlayerPreviewMeshAdmissionCandidate{ .key = *prewarm, .current_near = false };
    }
    if (!pending.prepareReady(maintenance_budget)) {
        return std::nullopt;
    }
    return PlayerPreviewMeshAdmissionCandidate{ .key = pending.top(), .current_near = false };
}

size_t PlayerPreviewMeshQueue::size() const noexcept
{
    return m_tickets.size();
}

HeightTileKey PlayerPreviewMeshQueue::top() const noexcept
{
    if (!hasReady()) {
        std::abort();
    }
    return m_heap.front().key;
}

bool PlayerPreviewMeshQueue::erase(HeightTileKey const key) noexcept
{
    return m_retirement->eraseTicket(m_tickets, key);
}

void PlayerPreviewMeshQueue::pop()
{
    ASSERT(hasReady(), "Preview mesh pop requires a current ready heap entry");
    Entry const entry = m_heap.front();
    std::pop_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    m_heap.pop_back();
    if (isCurrent(entry)) {
        static_cast<void>(m_retirement->eraseTicket(m_tickets, entry.key));
    }
}

PlayerPreviewMeshQueue::Entry PlayerPreviewMeshQueue::entryFor(
    HeightTileKey const key,
    uint64_t const ticket
) const noexcept
{
    return {
        .priority = shared::heightTileInterestPriority(m_center, m_heading_x, m_heading_y, key),
        .key = key,
        .ticket = ticket,
    };
}

bool PlayerPreviewMeshQueue::isCurrent(Entry const& entry) const noexcept
{
    auto const ticket = m_tickets.find(entry.key);
    return ticket != m_tickets.end() && ticket->second == entry.ticket;
}

uint32_t PlayerPreviewMeshQueue::pruneReadyHeap(uint32_t const maximum_entries) noexcept
{
    uint32_t inspected = 0U;
    while (inspected < maximum_entries && !m_heap.empty()) {
        ++inspected;
        if (isCurrent(m_heap.front())) {
            break;
        }
        std::pop_heap(m_heap.begin(), m_heap.end(), lowerPriority);
        m_heap.pop_back();
    }
    return inspected;
}

bool PlayerPreviewMeshQueue::lowerPriority(Entry const& first, Entry const& second) noexcept
{
    return std::tuple{ first.priority, first.key.y, first.key.x }
        > std::tuple{ second.priority, second.key.y, second.key.x };
}

bool playerClientCaptureCoverageComplete(
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& interest,
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& visible_meshes,
    PreviewResidency const& residency,
    uint32_t const expected_tile_count,
    size_t const pending_preview_removals
) noexcept
{
    if (pending_preview_removals != 0U
        || interest.size() != expected_tile_count
        || visible_meshes.size() != interest.size()
        || residency.stats().resident_tiles < expected_tile_count
        || residency.pendingChangeCount() != 0U) {
        return false;
    }
    for (HeightTileKey const key : interest) {
        if (!residency.resident(key) || !visible_meshes.contains(key)) {
            return false;
        }
    }
    return true;
}

bool playerClientCaptureCoverageComplete(
    HeightTileInterestRegion const& interest,
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& visible_meshes,
    PreviewResidency const& residency,
    uint32_t const expected_tile_count,
    size_t const pending_preview_removals
)
{
    if (pending_preview_removals != 0U
        || interest.size() != expected_tile_count
        || visible_meshes.size() != interest.size()
        || residency.stats().resident_tiles < expected_tile_count
        || residency.pendingChangeCount() != 0U) {
        return false;
    }
    if (interest.empty()) {
        return expected_tile_count == 0U;
    }
    std::optional<HeightTileKey> const center = interest.center();
    if (!center.has_value()) {
        return false;
    }
    int32_t const radius = static_cast<int32_t>(interest.radius());
    uint32_t covered_count = 0U;
    for (int32_t y = -radius; y <= radius; ++y) {
        for (int32_t x = -radius; x <= radius; ++x) {
            HeightTileKey const key = offsetHeightTileKey(*center, x, y);
            if (!interest.contains(key)) {
                continue;
            }
            ++covered_count;
            if (!residency.resident(key) || !visible_meshes.contains(key)) {
                return false;
            }
        }
    }
    return covered_count == interest.size();
}

void PlayerClient::refreshHeightTileInterest(
    shared::Player const& player,
    shared::HeightTileSurfaceProjection const projection, bool const capture_diagnostics)
{
    std::optional<uint32_t> const radius = renderDistance();
    if (!radius) {
        return;
    }
    if (m_interest_storage_radius != *radius) {
        uint32_t const required_tiles = shared::heightTileInterestCount(*radius);
        m_interest_delta_scratch.additions.reserve(required_tiles);
        m_interest_delta_scratch.removals.reserve(required_tiles);
        m_remesh_closure_scratch.reserve(static_cast<uint64_t>(required_tiles) * 5U);
        m_pending_keys_scratch.reserve(required_tiles);
        m_pending_preview_meshes.reserveStorage(required_tiles);
        m_interest_storage_radius = *radius;
    }
    CameraProjection camera_projection = m_camera.projection();
    double const far_plane = static_cast<double>(*radius) * shared::HEIGHT_TILE_SIDE_LENGTH
        + shared::WorldExtent::DEPTH;
    if (camera_projection.far_plane != far_plane) {
        camera_projection.far_plane = far_plane;
        static_cast<void>(m_camera.setProjection(camera_projection));
    }
    shared::HeightTileKey const center = shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(player.x),
        .y = floorDivideByHeightTileSide(player.y),
    });
    std::optional<shared::HeightTileKey> forecast_center;
    if (auto const forecast = forecastedLocalPlayerPosition()) {
        forecast_center = shared::normalizeHeightTileKey({
            .x = floorDivideByHeightTileSide(forecast->x),
            .y = floorDivideByHeightTileSide(forecast->y),
        });
    }
    bool const forecast_changed = forecast_center != m_preview_forecast_center;
    m_preview_forecast_center = forecast_center;
    shared::HeightTileHeading const heading = shared::canonicalHeightTileHeading(
        m_interest_heading_x, m_interest_heading_y
    );
    double const tile_origin_x = static_cast<double>(center.x) * shared::HEIGHT_TILE_SIDE_LENGTH;
    double const tile_origin_y = static_cast<double>(center.y) * shared::HEIGHT_TILE_SIDE_LENGTH;
    double constexpr PLAYER_WIDTH = static_cast<double>(shared::World::PLAYER_WIDTH_SUBCELLS)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    double constexpr PLAYER_HEIGHT = static_cast<double>(shared::World::PLAYER_HEIGHT_SUBCELLS)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    double const player_x = shared::playerPositionX(player);
    double const player_y = shared::playerPositionY(player);
    double const player_z = shared::playerPositionZ(player);
    shared::HeightTileSurfaceBounds const viewer_bounds{
        .min_x_blocks = player_x - tile_origin_x,
        .max_x_blocks = player_x - tile_origin_x + PLAYER_WIDTH,
        .min_y_blocks = player_y - tile_origin_y,
        .max_y_blocks = player_y - tile_origin_y + PLAYER_WIDTH,
        .min_z_blocks = player_z,
        .max_z_blocks = player_z + PLAYER_HEIGHT,
    };
    std::optional<HeightTileKey> const previous_center = m_height_tile_interest.center();
    bool const center_changed = !previous_center.has_value() || *previous_center != center;
    bool const heading_changed = m_applied_interest_heading_x != heading.x
        || m_applied_interest_heading_y != heading.y;
    if (center_changed || heading_changed) {
        m_pending_preview_meshes.beginPriorityRefresh(center, heading.x, heading.y);
    }
    if (center_changed) {
        shared::HeightTileInterestDelta& delta = m_interest_delta_scratch;
        if (previous_center.has_value()) {
            shared::heightTileInterestDelta(*previous_center, center, *radius, delta);
        } else {
            auto const initial = shared::makeHeightTileInterest(center, heading.x, heading.y, *radius);
            delta.additions.assign(initial.keys.begin(), initial.keys.end());
            delta.removals.clear();
        }
        for (HeightTileKey const key : delta.removals) {
            m_preview_mesh_coverage.interestRemoved(m_visible_preview_meshes.contains(key));
            static_cast<void>(m_pending_preview_meshes.erase(key));
            m_preview_tile_elevations.erase(key);
            queuePreviewRemoval(key);
        }
        m_preview_lod.eraseBatch(delta.removals);
        for (HeightTileKey const key : delta.additions) {
            m_preview_mesh_coverage.interestAdded(m_visible_preview_meshes.contains(key));
        }
        m_height_tile_interest.set(center, *radius);
        queuePreviewMeshes(delta.additions);
    }
    m_applied_interest_heading_x = heading.x;
    m_applied_interest_heading_y = heading.y;
    m_preview_mesh_projection = projection;
    if (center_changed || heading_changed || forecast_changed) {
        auto const priority_started = std::chrono::steady_clock::now();
        if (m_capture.has_value()) {
            m_capture_mesh_reprioritize_micros += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - priority_started
                ).count()
            );
        }
        m_preview_mesh_workers->setPriority(center, heading.x, heading.y, forecast_center);
        if (center_changed) {
            for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(
                     m_height_tile_interest
                 )) {
                m_preview_mesh_jobs.erase(key);
                m_preview_mesh_dirty_jobs.erase(key);
            }
        }
    }
    std::chrono::steady_clock::time_point const lod_refresh_started_at =
        capture_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    PlayerPreviewLodRefresh const refresh = m_preview_lod.refresh(center, viewer_bounds, projection);
    if (capture_diagnostics) {
        m_render_diagnostics.preview_lod_refresh_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - lod_refresh_started_at);
        m_render_diagnostics.preview_lod_classified_bucket_count = refresh.classified_buckets;
        m_render_diagnostics.preview_lod_evaluated_tile_count = refresh.evaluated_tiles;
        m_render_diagnostics.preview_lod_changed_tile_count = static_cast<uint32_t>(refresh.changed_keys.size());
    }
    playerPreviewLodRemeshClosure(refresh.changed_keys, m_remesh_closure_scratch);
    queuePreviewMeshes(m_remesh_closure_scratch);
}

std::array<HeightTileHandle, 4> PlayerClient::previewMeshNeighbors(HeightTileKey const key) const
{
    return {
        heightTileResidency().resident(offsetHeightTileKey(key, -1, 0)),
        heightTileResidency().resident(offsetHeightTileKey(key, 1, 0)),
        heightTileResidency().resident(offsetHeightTileKey(key, 0, -1)),
        heightTileResidency().resident(offsetHeightTileKey(key, 0, 1)),
    };
}

std::array<shared::HeightTileSurfaceDetail, 4> PlayerClient::previewMeshNeighborDetails(
    HeightTileKey const key
) const
{
    std::array<HeightTileKey, 4> const neighbor_keys{
        offsetHeightTileKey(key, -1, 0),
        offsetHeightTileKey(key, 1, 0),
        offsetHeightTileKey(key, 0, -1),
        offsetHeightTileKey(key, 0, 1),
    };
    std::array<HeightTileHandle, 4> const neighbors = previewMeshNeighbors(key);
    std::array<shared::HeightTileSurfaceDetail, 4> details{
        shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine,
        shared::HeightTileSurfaceDetail::Fine,
    };
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        if (neighbors[index]) {
            details[index] = m_preview_lod.detail(neighbor_keys[index])
                .value_or(shared::HeightTileSurfaceDetail::Fine);
        }
    }
    return details;
}

std::array<uint64_t, 4> PlayerClient::previewMeshNeighborProfileTokens(HeightTileKey const key) const
{
    std::array<HeightTileKey, 4> const neighbor_keys{
        offsetHeightTileKey(key, -1, 0),
        offsetHeightTileKey(key, 1, 0),
        offsetHeightTileKey(key, 0, -1),
        offsetHeightTileKey(key, 0, 1),
    };
    std::array<uint64_t, 4> tokens{};
    for (uint32_t edge = 0U; edge < neighbor_keys.size(); ++edge) {
        auto const installed = m_visible_preview_mesh_edge_profiles.find(neighbor_keys[edge]);
        if (installed != m_visible_preview_mesh_edge_profiles.end()) {
            tokens[edge] = installed->second.tokens[PreviewMeshSeamBridgeSet::OPPOSITE_EDGES[edge]];
        }
    }
    return tokens;
}

void PlayerClient::refreshPreviewMeshNeighborElevations(HeightTileKey const key)
{
    for (HeightTileKey const adjacent : {
             offsetHeightTileKey(key, -1, 0),
             offsetHeightTileKey(key, 1, 0),
             offsetHeightTileKey(key, 0, -1),
             offsetHeightTileKey(key, 0, 1),
         }) {
        HeightTileHandle const tile = heightTileResidency().resident(adjacent);
        if (!tile || !m_height_tile_interest.contains(adjacent)) {
            continue;
        }
        std::optional<shared::HeightTileSurfaceDetail> const previous = m_preview_lod.detail(adjacent);
        if (previewMeshDetail(adjacent, tile) == previous) {
            continue;
        }
        queuePreviewMesh(adjacent);
        for (HeightTileKey const dependent : {
                 offsetHeightTileKey(adjacent, -1, 0),
                 offsetHeightTileKey(adjacent, 1, 0),
                 offsetHeightTileKey(adjacent, 0, -1),
                 offsetHeightTileKey(adjacent, 0, 1),
             }) {
            queuePreviewMesh(dependent);
        }
    }
}

bool PlayerClient::refreshVisiblePreviewMesh(
    HeightTileKey const key,
    std::chrono::steady_clock::time_point const deadline,
    PreviewMeshSeamBridgeSet const* candidate_bridges
)
{
    auto const base = m_visible_preview_mesh_bases.find(key);
    if (base == m_visible_preview_mesh_bases.end()) {
        return false;
    }
    PreviewMeshSeamBridgeSet const* bridges = candidate_bridges;
    if (bridges == nullptr) {
        auto const seams = m_visible_preview_mesh_seam_bridges.find(key);
        if (seams != m_visible_preview_mesh_seam_bridges.end()) {
            bridges = &seams->second;
        }
    }
    shared::HeightTileSurfaceMesh mesh = bridges == nullptr ? base->second : bridges->compose(base->second);
    if (m_renderer.upsertHeightTileMesh(mesh, deadline)) {
        markPreviewMeshVisible(key);
        if (candidate_bridges != nullptr) {
            m_preview_cache_retirement_reaper->extractReplacedSeamBridges(*this, key);
            m_visible_preview_mesh_seam_bridges.insert_or_assign(key, *candidate_bridges);
            submitPreviewCacheRetirements();
        }
        return true;
    } else {
        if (m_capture.has_value()) {
            ++m_capture_mesh_publish_failures;
        }
        queuePreviewMesh(key);
        return false;
    }
}

bool PlayerClient::publishPreviewMesh(
    HeightTileKey const key,
    shared::HeightTileSurfaceMesh &mesh,
    HeightTileHandle const& tile,
    std::chrono::steady_clock::time_point const deadline
)
{
    bool const capture_mesh_diagnostics = m_benchmark.has_value() && m_benchmark->enable_mesh_stage_diagnostics;
    std::chrono::steady_clock::time_point const seam_planning_started_at =
        capture_mesh_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    PreviewMeshSeamBridgeSet center_bridges;
    auto const current_bridges = m_visible_preview_mesh_seam_bridges.find(key);
    if (current_bridges != m_visible_preview_mesh_seam_bridges.end()) {
        center_bridges = current_bridges->second;
    }
    std::array<HeightTileKey, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> const adjacent_keys{
        offsetHeightTileKey(key, -1, 0),
        offsetHeightTileKey(key, 1, 0),
        offsetHeightTileKey(key, 0, -1),
        offsetHeightTileKey(key, 0, 1),
    };
    std::array<PreviewMeshSeamNeighborSnapshot, MAX_PREVIEW_MESH_NEIGHBOR_UPSERTS_PER_RESULT> neighbors;
    for (uint32_t index = 0U; index < adjacent_keys.size(); ++index) {
        auto const neighbor_base = m_visible_preview_mesh_bases.find(adjacent_keys[index]);
        if (neighbor_base != m_visible_preview_mesh_bases.end()) {
            auto const neighbor_bridges = m_visible_preview_mesh_seam_bridges.find(adjacent_keys[index]);
            neighbors[index] = {
                .mesh = &neighbor_base->second,
                .bridges = neighbor_bridges == m_visible_preview_mesh_seam_bridges.end()
                    ? nullptr
                    : &neighbor_bridges->second,
            };
        }
    }
    PreviewMeshSeamPublicationPlan plan = planPreviewMeshSeamPublication(
        mesh,
        center_bridges,
        neighbors
    );
    PreviewMeshSeamPublicationBatch const batch = buildPreviewMeshSeamPublicationBatch(plan, neighbors);
    std::array<shared::HeightTileSurfaceEdgeProfile, 4> edge_profiles;
    for (uint32_t edge = 0U; edge < edge_profiles.size(); ++edge) {
        edge_profiles[edge] = shared::heightTileSurfaceEdgeProfile(
            mesh, PreviewMeshSeamBridgeSet::EDGES[edge]
        );
    }
    if (capture_mesh_diagnostics) {
        m_mesh_result_drain_diagnostics.seam_planning_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - seam_planning_started_at);
    }
    bool const published = publishPreviewMeshSeamPlan(
        plan,
        batch,
        [this, deadline, capture_mesh_diagnostics](std::span<shared::HeightTileSurfaceMesh const> candidates) {
            std::chrono::steady_clock::time_point const upsert_started_at =
                capture_mesh_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            RendererHeightTileUpsertDiagnostics upsert_diagnostics;
            bool const upserted = m_renderer.upsertHeightTileMeshes(
                candidates,
                deadline,
                capture_mesh_diagnostics ? &upsert_diagnostics : nullptr
            );
            if (capture_mesh_diagnostics) {
                m_mesh_result_drain_diagnostics.renderer_batch_upsert_duration +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - upsert_started_at);
                m_mesh_result_drain_diagnostics.renderer_upsert_stages.free_range_prepare_duration +=
                    upsert_diagnostics.free_range_prepare_duration;
                m_mesh_result_drain_diagnostics.renderer_upsert_stages.mesh_prepare_duration +=
                    upsert_diagnostics.mesh_prepare_duration;
                m_mesh_result_drain_diagnostics.renderer_upsert_stages.capacity_prepare_duration +=
                    upsert_diagnostics.capacity_prepare_duration;
                m_mesh_result_drain_diagnostics.renderer_upsert_stages.commit_upload_duration +=
                    upsert_diagnostics.commit_upload_duration;
            }
            return upserted;
        },
        [this, key, &mesh, &tile, &adjacent_keys, &edge_profiles](
            PreviewMeshSeamPublicationPlan& accepted_plan
        ) {
            markPreviewMeshVisible(key);
            m_preview_cache_retirement_reaper->extractReplacedBase(*this, key);
            m_visible_preview_mesh_bases.insert_or_assign(key, std::move(mesh));
            auto const previous = m_visible_preview_mesh_edge_profiles.find(key);
            PreviewMeshEdgeProfiles profiles{ .edges = edge_profiles };
            for (uint32_t edge = 0U; edge < edge_profiles.size(); ++edge) {
                profiles.tokens[edge] = previous != m_visible_preview_mesh_edge_profiles.end()
                        && previous->second.edges[edge].top == edge_profiles[edge].top
                    ? previous->second.tokens[edge]
                    : ++m_preview_edge_profile_token;
            }
            m_preview_cache_retirement_reaper->extractReplacedEdgeProfiles(*this, key);
            m_visible_preview_mesh_edge_profiles.insert_or_assign(key, profiles);
            m_preview_cache_retirement_reaper->extractReplacedSeamBridges(*this, key);
            m_visible_preview_mesh_seam_bridges.insert_or_assign(key, std::move(accepted_plan.center_bridges));
            m_preview_cache_retirement_reaper->extractReplacedTile(*this, key);
            m_visible_preview_mesh_tiles.insert_or_assign(key, tile);
            for (uint32_t index = 0U; index < accepted_plan.changed_neighbors.size(); ++index) {
                if (accepted_plan.changed_neighbors[index]) {
                    markPreviewMeshVisible(adjacent_keys[index]);
                    m_preview_cache_retirement_reaper->extractReplacedSeamBridges(
                        *this,
                        adjacent_keys[index]
                    );
                    m_visible_preview_mesh_seam_bridges.insert_or_assign(
                        adjacent_keys[index],
                        std::move(accepted_plan.neighbor_bridges[index])
                    );
                }
            }
            refreshPreviewMeshNeighborElevations(key);
        }
    );
    if (!published) {
        if (m_capture.has_value()) {
            ++m_capture_mesh_publish_failures;
        }
    }
    if (capture_mesh_diagnostics) {
        if (published) {
            ++m_mesh_result_drain_diagnostics.publish_success_count;
        } else {
            ++m_mesh_result_drain_diagnostics.publish_failure_count;
        }
    }
    return published;
}

void PlayerClient::removePreviewMesh(
    HeightTileKey const key,
    std::vector<shared::HeightTileCoordinate> &renderer_removal_batch)
{
    bool const capture_render_diagnostics = m_benchmark.has_value();
    renderer_removal_batch.push_back({ .x = key.x, .y = key.y });
    m_preview_cache_retirement_reaper->extractRemoved(*this, key);
    std::chrono::steady_clock::time_point const neighbor_enqueue_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (HeightTileKey const adjacent : {
             offsetHeightTileKey(key, -1, 0),
             offsetHeightTileKey(key, 1, 0),
             offsetHeightTileKey(key, 0, -1),
             offsetHeightTileKey(key, 0, 1),
         }) {
        queuePreviewMesh(adjacent);
    }
    if (capture_render_diagnostics) {
        std::chrono::steady_clock::time_point const remove_completed_at = std::chrono::steady_clock::now();
        m_render_diagnostics.preview_mesh_remove_neighbor_enqueue_duration +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(remove_completed_at - neighbor_enqueue_started_at);
    }
}

shared::HeightTileSurfaceDetail PlayerClient::previewMeshDetail(
    HeightTileKey const key,
    HeightTileHandle const& tile
)
{
    if (!tile || m_height_tile_interest.empty()) {
        return shared::HeightTileSurfaceDetail::Fine;
    }
    std::array<HeightTileHandle, 4> const neighbors = previewMeshNeighbors(key);
    std::array<HeightTileKey, 4> const neighbor_keys{
        offsetHeightTileKey(key, -1, 0),
        offsetHeightTileKey(key, 1, 0),
        offsetHeightTileKey(key, 0, -1),
        offsetHeightTileKey(key, 0, 1),
    };
    std::array<uint64_t, 4> neighbor_tokens{};
    std::array<shared::HeightTileSurfaceDetail, 4> const neighbor_details = previewMeshNeighborDetails(key);
    std::array<uint64_t, 4> const neighbor_profile_tokens = previewMeshNeighborProfileTokens(key);
    std::array<std::optional<shared::HeightTileSurfaceEdgeProfile>, 4> installed_neighbor_profiles{};
    std::array<std::optional<shared::HeightTileSurfaceElevationRange>, 4> neighbor_mesh_ranges{};
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        if (neighbors[index]) {
            neighbor_tokens[index] = neighbors[index]->token();
        }
        auto const installed = m_visible_preview_mesh_edge_profiles.find(neighbor_keys[index]);
        if (installed != m_visible_preview_mesh_edge_profiles.end()) {
            shared::HeightTileSurfaceEdgeProfile const &installed_profile = installed->second.edges[
                PreviewMeshSeamBridgeSet::OPPOSITE_EDGES[index]
            ];
            installed_neighbor_profiles[index] = installed_profile;
            neighbor_mesh_ranges[index] = installed_profile.elevation;
        }
    }
    auto elevation = m_preview_tile_elevations.find(key);
    if (elevation == m_preview_tile_elevations.end()
        || elevation->second.tile_revision != tile->revision()
        || elevation->second.tile_token != tile->token()
        || elevation->second.neighbor_tokens != neighbor_tokens
        || elevation->second.neighbor_details != neighbor_details
        || elevation->second.neighbor_profile_tokens != neighbor_profile_tokens
        || elevation->second.neighbor_mesh_ranges != neighbor_mesh_ranges) {
        uint16_t minimum = std::numeric_limits<uint16_t>::max();
        uint16_t maximum = 0U;
        auto const includeHeight = [&minimum, &maximum](uint16_t const height) {
            minimum = std::min(minimum, height);
            maximum = std::max(maximum, height);
        };
        for (uint16_t const height : tile->heights()) {
            includeHeight(height);
        }
        for (size_t index = 0U; index < neighbors.size(); ++index) {
            if (neighbors[index]) {
                for (uint16_t const height : neighbors[index]->heights()) {
                    includeHeight(height);
                }
            } else {
                includeHeight(0U);
            }
            auto const& neighbor_mesh_range = neighbor_mesh_ranges[index];
            if (neighbor_mesh_range.has_value()) {
                includeHeight(neighbor_mesh_range.value().minimum);
                includeHeight(neighbor_mesh_range.value().maximum);
            }
        }
        auto const heights = [](HeightTileHandle const& neighbor) -> std::optional<shared::HeightTile::Heights> {
            return neighbor ? std::optional{neighbor->heights()} : std::nullopt;
        };
        shared::HeightTileSurfaceNeighbors const surface_neighbors{
            .negative_x = heights(neighbors[0]),
            .positive_x = heights(neighbors[1]),
            .negative_y = heights(neighbors[2]),
            .positive_y = heights(neighbors[3]),
            .negative_x_detail = neighbor_details[0],
            .positive_x_detail = neighbor_details[1],
            .negative_y_detail = neighbor_details[2],
            .positive_y_detail = neighbor_details[3],
        };
        shared::HeightTileSurfaceFaceAreas const face_areas = shared::heightTileSurfaceFaceAreas({
            .coordinate = { .x = key.x, .y = key.y },
            .heights = tile->heights(),
        }, surface_neighbors, installed_neighbor_profiles);
        elevation = m_preview_tile_elevations.insert_or_assign(key, PreviewTileElevation{
            .tile_revision = tile->revision(),
            .tile_token = tile->token(),
            .neighbor_tokens = neighbor_tokens,
            .neighbor_details = neighbor_details,
            .neighbor_profile_tokens = neighbor_profile_tokens,
            .neighbor_mesh_ranges = neighbor_mesh_ranges,
            .minimum = minimum,
            .maximum = maximum,
            .face_areas = face_areas,
        }).first;
    }
    std::optional<shared::HeightTileSurfaceDetail> const previous = m_preview_lod.detail(key);
    shared::HeightTileSurfaceDetail const selected = m_preview_lod.selectDetail(key,
        elevation->second.minimum, elevation->second.maximum, elevation->second.face_areas);
    if (previous.value_or(shared::HeightTileSurfaceDetail::Fine) != selected) {
        for (HeightTileKey const adjacent : neighbor_keys) {
            queuePreviewMesh(adjacent);
        }
    }
    return selected;
}

void PlayerClient::queuePreviewMesh(HeightTileKey const key)
{
    if (!m_height_tile_interest.contains(key) || !heightTileResidency().resident(key)) {
        return;
    }
    if (m_preview_mesh_jobs.contains(key)) {
        m_preview_mesh_dirty_jobs.insert(key);
    } else {
        static_cast<void>(m_pending_preview_meshes.push(key));
    }
}

void PlayerClient::queuePreviewMeshes(std::span<HeightTileKey const> const keys)
{
    std::vector<HeightTileKey>& pending = m_pending_keys_scratch;
    pending.clear();
    for (HeightTileKey const key : keys) {
        if (!m_height_tile_interest.contains(key) || !heightTileResidency().resident(key)) {
            continue;
        }
        if (m_preview_mesh_jobs.contains(key)) {
            m_preview_mesh_dirty_jobs.insert(key);
        } else {
            pending.push_back(key);
        }
    }
    m_pending_preview_meshes.pushBatch(pending);
}

void PlayerClient::queuePreviewRemoval(HeightTileKey const key)
{
    if (!playerClientShouldQueuePreviewRemoval(m_capture.has_value(), m_visible_preview_meshes.contains(key))) {
        return;
    }
    if (m_pending_preview_removal_set.insert(key).second) {
        m_pending_preview_removals.push_back(key);
    }
}

void PlayerClient::markPreviewMeshVisible(HeightTileKey const key)
{
    bool const newly_visible = m_visible_preview_meshes.insert(key).second;
    m_preview_mesh_coverage.meshPublished(newly_visible, m_height_tile_interest.contains(key));
}

bool PlayerClient::hasCurrentPreviewMeshCoverage() const noexcept
{
    return m_preview_mesh_coverage.complete(static_cast<uint32_t>(m_height_tile_interest.size()));
}

bool PlayerClient::hasExactCurrentPreviewMeshCoverage() const noexcept
{
    if (!renderDistance() || !m_pending_preview_meshes.empty()
        || !m_preview_mesh_jobs.empty()
        || !m_preview_mesh_dirty_jobs.empty()) {
        return false;
    }
    return playerClientCaptureCoverageComplete(
        m_height_tile_interest,
        m_visible_preview_meshes,
        heightTileResidency(),
        requiredHeightTileCount(),
        m_pending_preview_removals.size()
    );
}

bool PlayerClient::hasExactCurrentRendererMeshCoverage() const
{
    return hasExactCurrentPreviewMeshCoverage()
        && m_renderer.runtimeInfo().height_tile_mesh_count == requiredHeightTileCount();
}

bool PlayerClient::enqueuePreviewMeshJob(
    HeightTileKey const key,
    HeightTileHandle const& tile,
    HeightTileKey const center,
    std::span<HeightTileKey const> const nearby_candidates,
    std::chrono::steady_clock::time_point const near_admission_deadline
)
{
    if (std::chrono::steady_clock::now() >= near_admission_deadline) {
        return false;
    }
    std::array<HeightTileHandle, 4> const neighbors = previewMeshNeighbors(key);
    shared::HeightTileSurfaceDetail const target_detail = previewMeshDetail(key, tile);
    auto const visible_tile = m_visible_preview_mesh_tiles.find(key);
    PreviewMeshStage const stage = previewMeshStageForTile(
        visible_tile == m_visible_preview_mesh_tiles.end() ? HeightTileHandle{} : visible_tile->second,
        tile,
        target_detail
    );
    shared::HeightTileSurfaceDetail const mesh_detail = stage == PreviewMeshStage::Coarse
        ? previewMeshCoarseDetail(target_detail)
        : target_detail;
    PreviewMeshWorkerPool::Job job{
        .key = key,
        .source = {
            .tile = tile,
            .neighbors = neighbors,
            .detail = mesh_detail,
            .neighbor_details = previewMeshNeighborDetails(key),
        },
        .target_detail = target_detail,
        .stage = stage,
        .epoch = m_preview_mesh_epoch,
    };
    if (std::chrono::steady_clock::now() >= near_admission_deadline) {
        return false;
    }
    bool const current_near_key = withinCurrentNearProbe({
        .center = center,
        .key = key,
    });
    bool near_pending_before_enqueue = false;
    if (m_benchmark.has_value() && !current_near_key) {
        near_pending_before_enqueue = std::any_of(
            nearby_candidates.begin(), nearby_candidates.end(),
            [this](HeightTileKey const candidate) {
                return m_pending_preview_meshes.contains(candidate);
            }
        );
    }
    if (!m_preview_mesh_workers->enqueue(std::move(job))) {
        return false;
    }
    if (m_benchmark.has_value()) {
        if (current_near_key) {
            ++m_benchmark_accepted_near_admissions;
        } else {
            ++m_benchmark_accepted_far_admissions;
            if (near_pending_before_enqueue) {
                ++m_benchmark_far_admissions_while_near_pending;
            }
        }
    }
    m_preview_mesh_jobs.insert(key);
    static_cast<void>(m_pending_preview_meshes.erase(key));
    return true;
}

void PlayerClient::processPendingPreviewMeshes(
    uint32_t const maximum_meshes,
    std::chrono::steady_clock::time_point const mesh_work_deadline)
{
    if (m_deferred_preview_mesh_result_deadline == mesh_work_deadline) {
        return;
    }
    m_deferred_preview_mesh_result_deadline.reset();
    bool const capture_mesh_diagnostics = m_benchmark.has_value() && m_benchmark->enable_mesh_stage_diagnostics;
    std::optional<HeightTileKey> const center = m_height_tile_interest.center();
    if (!center.has_value()) {
        while (std::optional<PreviewMeshWorkerPool::Result> result = m_preview_mesh_workers->takeResult()) {
            if (capture_mesh_diagnostics) {
                ++m_mesh_result_drain_diagnostics.drained_result_count;
                ++m_mesh_result_drain_diagnostics.no_longer_interested_or_resident_discard_count;
            }
            m_preview_mesh_workers->finishResult(result->key);
            m_preview_mesh_jobs.erase(result->key);
            m_preview_mesh_workers->retireTakenResult(std::move(*result));
        }
        m_preview_mesh_workers->submitRetired();
        return;
    }
    static constexpr uint32_t PRIORITY_REFRESH_BUDGET = 256U;
    uint32_t maintenance_budget = PRIORITY_REFRESH_BUDGET;
    maintenance_budget -= m_pending_preview_meshes.refreshPriority(maintenance_budget, m_height_tile_interest);
    auto const upload_started = std::chrono::steady_clock::now();
    for (uint32_t completed = 0U; completed < maximum_meshes
         && std::chrono::steady_clock::now() < mesh_work_deadline; ++completed) {
        std::optional<PreviewMeshWorkerPool::Result> result = m_preview_mesh_workers->takeResult();
        if (!result.has_value()) {
            break;
        }
        if (capture_mesh_diagnostics) {
            ++m_mesh_result_drain_diagnostics.drained_result_count;
        }
        if (result->epoch != m_preview_mesh_epoch) {
            if (capture_mesh_diagnostics) {
                ++m_mesh_result_drain_diagnostics.stale_epoch_discard_count;
            }
            m_preview_mesh_workers->finishResult(result->key);
            m_preview_mesh_jobs.erase(result->key);
            m_preview_mesh_workers->retireTakenResult(std::move(*result));
            continue;
        }
        HeightTileHandle const current = heightTileResidency().resident(result->key);
        std::array<HeightTileHandle, 4> current_neighbors{};
        bool const currently_interested = m_height_tile_interest.contains(result->key);
        bool source_current = false;
        bool detail_current = false;
        if (currently_interested && current) {
            current_neighbors = previewMeshNeighbors(result->key);
            source_current = result->source.matchesGeometry(current, current_neighbors,
                previewMeshNeighborDetails(result->key));
            shared::HeightTileSurfaceDetail const expected_mesh_detail = result->stage
                    == PreviewMeshStage::Coarse
                ? previewMeshCoarseDetail(result->target_detail)
                : result->target_detail;
            // Installed profiles affect certification, not base geometry. Revalidate
            // their current content before rebuilding seams from the installed
            // meshes.
            detail_current = previewMeshDetail(result->key, current) == result->target_detail
                && result->mesh.detail == expected_mesh_detail;
        }
        HeightTileHandle visible_tile;
        auto const visible = m_visible_preview_mesh_tiles.find(result->key);
        if (visible != m_visible_preview_mesh_tiles.end()) {
            visible_tile = visible->second;
        }
        bool const can_publish = previewMeshCanPublish(
            visible_tile,
            result->source.tile,
            result->stage,
            current
        );
        if (capture_mesh_diagnostics) {
            if (!currently_interested || !current) {
                ++m_mesh_result_drain_diagnostics.no_longer_interested_or_resident_discard_count;
            } else if (!source_current) {
                ++m_mesh_result_drain_diagnostics.source_mismatch_requeue_count;
            } else if (!detail_current) {
                ++m_mesh_result_drain_diagnostics.target_detail_mismatch_requeue_count;
            } else if (!can_publish) {
                ++m_mesh_result_drain_diagnostics.cannot_publish_requeue_count;
            }
        }
        bool published = false;
        if (source_current && detail_current && can_publish) {
            if (capture_mesh_diagnostics) {
                ++m_mesh_result_drain_diagnostics.publish_attempt_count;
            }
            published = publishPreviewMesh(
                result->key, result->mesh,
                result->source.tile, mesh_work_deadline);
            if (!published && !m_preview_mesh_dirty_jobs.contains(result->key)) {
                m_preview_mesh_workers->requeueResult(std::move(*result));
                m_deferred_preview_mesh_result_deadline = mesh_work_deadline;
                break;
            }
        }

        bool const retry_coarse =
            source_current && detail_current && !can_publish && result->stage == PreviewMeshStage::Coarse;
        bool const retry_dirty =
            m_preview_mesh_dirty_jobs.erase(result->key) > 0U || !source_current || !detail_current;
        bool const refine_coarse = published && result->stage == PreviewMeshStage::Coarse
                && m_visible_preview_mesh_tiles.contains(result->key);
        m_preview_mesh_workers->finishResult(result->key);
        m_preview_mesh_jobs.erase(result->key);
        if (m_capture.has_value()) {
            ++m_capture_mesh_results;
        }
        if (retry_coarse || retry_dirty || refine_coarse) {
            queuePreviewMesh(result->key);
        }
        if (published && source_current && result->mesh.quads.capacity() == 0U) {
            // Exact source handles remain owned by current residency and these pinned snapshots.
            result->source = {};
        } else {
            m_preview_mesh_workers->retireTakenResult(std::move(*result));
        }
        if (std::chrono::steady_clock::now() - upload_started >= std::chrono::milliseconds{2}) {
            break;
        }
    }
    if (m_capture.has_value()) {
        m_capture_mesh_drain_micros += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - upload_started
            ).count()
        );
    }
    std::array<HeightTileKey, PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> nearby_candidates{};
    uint32_t nearby_candidate_count = 0U;
    bool nearby_probe_complete = true;
    for (PlayerClientPreviewMeshProbeOffset const offset : PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
            if (std::chrono::steady_clock::now() >= mesh_work_deadline) {
                nearby_probe_complete = false;
                break;
            }
            HeightTileKey const key = offsetHeightTileKey(*center, offset.x, offset.y);
            if (!m_height_tile_interest.contains(key) || m_preview_mesh_jobs.contains(key)) {
                continue;
            }
            HeightTileHandle const tile = heightTileResidency().resident(key);
            if (!tile) {
                continue;
            }
            auto const visible = m_visible_preview_mesh_tiles.find(key);
            bool const current_mesh_visible = visible != m_visible_preview_mesh_tiles.end()
                && visible->second == tile;
            if (!current_mesh_visible) {
                queuePreviewMesh(key);
                nearby_candidates[nearby_candidate_count++] = key;
            }
        }
    std::array<HeightTileKey, PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT> forecast_candidates{};
    uint32_t forecast_candidate_count = 0U;
    bool forecast_probe_complete = nearby_probe_complete;
    if (forecast_probe_complete && m_preview_forecast_center
        && *m_preview_forecast_center != *center) {
        for (PlayerClientPreviewMeshProbeOffset const offset : PLAYER_CLIENT_PREVIEW_MESH_PROBE_OFFSETS) {
            if (std::chrono::steady_clock::now() >= mesh_work_deadline) {
                forecast_probe_complete = false;
                break;
            }
            HeightTileKey const key = offsetHeightTileKey(*m_preview_forecast_center, offset.x, offset.y);
            if (!m_height_tile_interest.contains(key) || m_preview_mesh_jobs.contains(key)
                || withinCurrentNearProbe({ .center = *center, .key = key })) {
                continue;
            }
            HeightTileHandle const tile = heightTileResidency().resident(key);
            if (!tile) {
                continue;
            }
            auto const visible = m_visible_preview_mesh_tiles.find(key);
            if (visible == m_visible_preview_mesh_tiles.end() || visible->second != tile) {
                queuePreviewMesh(key);
                forecast_candidates[forecast_candidate_count++] = key;
            }
        }
    }
    std::array<HeightTileKey, PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSET_COUNT> prewarm_candidates{};
    uint32_t prewarm_candidate_count = 0U;
    bool prewarm_probe_complete = forecast_probe_complete;
    if (prewarm_probe_complete) {
        for (PlayerClientPreviewMeshProbeOffset const offset : PLAYER_CLIENT_PREVIEW_MESH_PREWARM_OFFSETS) {
            if (std::chrono::steady_clock::now() >= mesh_work_deadline) {
                prewarm_probe_complete = false;
                break;
            }
            HeightTileKey const key = offsetHeightTileKey(*center, offset.x, offset.y);
            if (m_height_tile_interest.contains(key)) {
                prewarm_candidates[prewarm_candidate_count++] = key;
            }
        }
    }
    std::span<HeightTileKey const> const nearby_candidate_span{
        nearby_candidates.data(), nearby_candidate_count
    };
    std::span<HeightTileKey const> const prewarm_candidate_span{ prewarm_candidates.data(), prewarm_candidate_count };
    std::span<HeightTileKey const> const forecast_candidate_span{
        forecast_candidates.data(), forecast_candidate_count
    };
    uint32_t const admission_budget = playerClientPreviewMeshAdmissionBudget(m_capture.has_value());
    for (uint32_t examined = 0U;
         examined < admission_budget && std::chrono::steady_clock::now() < mesh_work_deadline;
         ++examined) {
        std::optional<PlayerPreviewMeshAdmissionCandidate> const admission_candidate =
            nextPlayerPreviewMeshAdmissionCandidate(m_pending_preview_meshes, maintenance_budget,
                nearby_candidate_span,
                nearby_probe_complete,
                prewarm_candidate_span,
                prewarm_probe_complete,
                forecast_candidate_span,
                forecast_probe_complete);
        if (!admission_candidate.has_value()) {
            break;
        }
        while ((admission_candidate->current_near || admission_candidate->forecast_near)
            && !m_preview_mesh_workers->canAccept()) {
            uint8_t const admitted_tier = admission_candidate->current_near ? 0U : 1U;
            std::optional<HeightTileKey> const deferred = m_preview_mesh_workers->deferLowerPriority(admitted_tier);
            if (!deferred.has_value()) {
                break;
            }
            m_preview_mesh_jobs.erase(*deferred);
            m_preview_mesh_dirty_jobs.erase(*deferred);
            queuePreviewMesh(*deferred);
        }
        if (!m_preview_mesh_workers->canAccept()) {
            break;
        }
        HeightTileKey const key = admission_candidate->key;
        HeightTileHandle const tile = heightTileResidency().resident(key);
        if (!m_height_tile_interest.contains(key) || !tile) {
            static_cast<void>(m_pending_preview_meshes.erase(key));
            continue;
        }
        if (m_preview_mesh_jobs.contains(key)) {
            static_cast<void>(m_pending_preview_meshes.erase(key));
            continue;
        }
        if (std::chrono::steady_clock::now() >= mesh_work_deadline) {
            break;
        }
        if (!enqueuePreviewMeshJob(
                key, tile, *center, nearby_candidate_span, mesh_work_deadline)) {
            break;
        }
    }
    m_preview_mesh_workers->submitRetired();
}

void PlayerClient::updateFlightControlToggles()
{
    bool const increase_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_EQUAL) == GLFW_PRESS
        || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_KP_ADD) == GLFW_PRESS;
    bool const decrease_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_MINUS) == GLFW_PRESS
        || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS;
    bool const increase = m_speedup_increase_latch.update(increase_pressed);
    bool const decrease = m_speedup_decrease_latch.update(decrease_pressed);
    if (increase != decrease) {
        if (increase) {
            m_speedup_profile_index = (m_speedup_profile_index + 1U)
                % shared::FLIGHT_SPEEDUP_PROFILES.size();
        } else {
            m_speedup_profile_index = (m_speedup_profile_index + shared::FLIGHT_SPEEDUP_PROFILES.size() - 1U)
                % shared::FLIGHT_SPEEDUP_PROFILES.size();
        }
    }
    bool const control_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
        || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    if (m_acceleration_toggle_latch.update(control_pressed)) {
        m_acceleration_enabled = !m_acceleration_enabled;
    }
}

shared::Direction PlayerClient::captureInput() noexcept
{
    if (!m_capture.has_value() || m_capture_failed || m_capture_succeeded) {
        return {};
    }
    std::optional<shared::Player> const player = predictedLocalPlayer();
    if (!player.has_value()) {
        return {};
    }
    PlayerClientCapturePlan const plan = playerClientCapturePlan(m_capture->preset);
    if (plan.final_movement_capabilities.has_value()
        && captureAtTarget(*player)
        && player->movement_capabilities != *plan.final_movement_capabilities) {
        if (m_capture_last_capability_cycle_source == player->movement_capabilities.bits) {
            return {};
        }
        m_capture_last_capability_cycle_source = player->movement_capabilities.bits;
        glm::dvec3 const view = m_look_camera.forward();
        return {
            .x = 0U,
            .y = 0U,
            .z = 0U,
            .cycle_movement_capabilities = true,
            .view_x = CameraController::quantize(view.x),
            .view_y = CameraController::quantize(view.y),
        };
    }
    m_capture_last_capability_cycle_source.reset();
    int64_t const current_x = static_cast<int64_t>(player->x) * shared::SUBCELLS_PER_CELL
        + player->x_subcell;
    int64_t const current_y = static_cast<int64_t>(player->y) * shared::SUBCELLS_PER_CELL
        + player->y_subcell;
    int64_t const current_z = static_cast<int64_t>(player->z) * shared::SUBCELLS_PER_CELL
        + player->z_subcell;
    int64_t const target_x = static_cast<int64_t>(plan.target.x) * shared::SUBCELLS_PER_CELL
        + plan.target.x_subcell;
    int64_t const target_y = static_cast<int64_t>(plan.target.y) * shared::SUBCELLS_PER_CELL
        + plan.target.y_subcell;
    int64_t const target_z = static_cast<int64_t>(plan.target.z) * shared::SUBCELLS_PER_CELL
        + plan.target.z_subcell;
    int64_t const delta_x = shortestFlightDelta(current_x, target_x);
    int64_t const delta_y = shortestFlightDelta(current_y, target_y);
    int64_t const delta_z = target_z - current_z;
    int64_t const largest_horizontal_delta = std::max(
        absoluteValue(delta_x),
        absoluteValue(delta_y)
    );
    bool const horizontal_arrival_pending = largest_horizontal_delta > CAPTURE_POSITION_TOLERANCE_SUBCELLS;
    int64_t const requested_x = horizontal_arrival_pending ? delta_x : 0;
    int64_t const requested_y = horizontal_arrival_pending ? delta_y : 0;
    int64_t const requested_z = horizontal_arrival_pending ? 0 : delta_z;
    int64_t const largest_delta = std::max({
        absoluteValue(requested_x),
        absoluteValue(requested_y),
        absoluteValue(requested_z),
    });
    uint16_t const speedup = playerClientCaptureSpeedup(largest_delta);
    auto const capture_axis_component = [speedup](int64_t const delta) noexcept {
        if (delta == 0) {
            return int8_t{0};
        }
        uint16_t const effective_speedup = speedup == CAPTURE_NEAR_SPEEDUP ? 1U : speedup;
        int64_t const maximum_step = static_cast<int64_t>(shared::MOVEMENT_SUBCELLS_PER_TICK)
            * effective_speedup;
        int64_t const magnitude = std::clamp(
            absoluteValue(delta) * 127 / maximum_step,
            int64_t{1},
            int64_t{127}
        );
        return static_cast<int8_t>(delta < 0 ? -magnitude : magnitude);
    };
    int8_t const x = capture_axis_component(requested_x);
    int8_t const y = capture_axis_component(requested_y);
    int8_t const z = capture_axis_component(requested_z);
    return {
        .x = static_cast<uint8_t>(x),
        .y = static_cast<uint8_t>(y),
        .z = static_cast<uint8_t>(z),
        .accelerated = speedup != CAPTURE_NEAR_SPEEDUP,
        .speedup = speedup,
        .view_x = x != 0 || y != 0
            ? static_cast<int8_t>(x)
            : CameraController::quantize(m_look_camera.forward().x),
        .view_y = x != 0 || y != 0
            ? static_cast<int8_t>(y)
            : CameraController::quantize(m_look_camera.forward().y),
    };
}

bool PlayerClient::captureAtTarget(shared::Player const& player) const noexcept
{
    if (!m_capture.has_value()) {
        return false;
    }
    PlayerClientCapturePlan const plan = playerClientCapturePlan(m_capture->preset);
    int64_t const current_x = static_cast<int64_t>(player.x) * shared::SUBCELLS_PER_CELL
        + player.x_subcell;
    int64_t const current_y = static_cast<int64_t>(player.y) * shared::SUBCELLS_PER_CELL
        + player.y_subcell;
    int64_t const current_z = static_cast<int64_t>(player.z) * shared::SUBCELLS_PER_CELL
        + player.z_subcell;
    int64_t const target_x = static_cast<int64_t>(plan.target.x) * shared::SUBCELLS_PER_CELL
        + plan.target.x_subcell;
    int64_t const target_y = static_cast<int64_t>(plan.target.y) * shared::SUBCELLS_PER_CELL
        + plan.target.y_subcell;
    int64_t const target_z = static_cast<int64_t>(plan.target.z) * shared::SUBCELLS_PER_CELL
        + plan.target.z_subcell;
    return absoluteValue(shortestFlightDelta(current_x, target_x)) <= CAPTURE_POSITION_TOLERANCE_SUBCELLS
        && absoluteValue(shortestFlightDelta(current_y, target_y)) <= CAPTURE_POSITION_TOLERANCE_SUBCELLS
        && absoluteValue(current_z - target_z) <= CAPTURE_POSITION_TOLERANCE_SUBCELLS;
}

bool PlayerClient::captureSceneReady() const
{
    if (!m_capture.has_value()) {
        return false;
    }
    PlayerClientCapturePlan const plan = playerClientCapturePlan(m_capture->preset);
    if (plan.final_movement_capabilities.has_value()) {
        std::optional<shared::Player> const player = predictedLocalPlayer();
        if (!player.has_value() || player->movement_capabilities != *plan.final_movement_capabilities) {
            return false;
        }
    }
    RendererRuntimeInfo const runtime = m_renderer.runtimeInfo();
    if (runtime.height_tile_mesh_count < m_capture->minimum_height_tile_meshes) {
        return false;
    }
    if (m_capture->preset == PlayerClientCapturePreset::FirstPersonOrigin) {
        static constexpr int32_t LAST_TILE = static_cast<int32_t>(
            shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH - 1U
        );
        for (HeightTileKey const key : {
            HeightTileKey{ .x = 0, .y = 0 },
            HeightTileKey{ .x = LAST_TILE, .y = 0 },
            HeightTileKey{ .x = 0, .y = LAST_TILE },
            HeightTileKey{ .x = LAST_TILE, .y = LAST_TILE },
        }) {
            if (!m_height_tile_interest.contains(key) || !m_visible_preview_meshes.contains(key)) {
                return false;
            }
        }
    }
    return hasExactCurrentRendererMeshCoverage();
}

void PlayerClient::failCapture(std::string_view const reason)
{
    if (!m_capture_failed) {
        PlayerClientCapturePreset const preset = m_capture
            ? m_capture->preset
            : PlayerClientCapturePreset::CentralSpike;
        PlayerClientCapturePlan const plan = playerClientCapturePlan(preset);
        CORE_ERROR("Player capture '{}' failed: {}", plan.name, reason);
    }
    m_capture_failed = true;
    m_running = false;
}

void PlayerClient::beginContinuousLook() noexcept
{
    glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

shared::Direction PlayerClient::input() {
    if (m_capture.has_value()) {
        shared::Direction const direction = captureInput();
        m_interest_heading_x = direction.view_x;
        m_interest_heading_y = direction.view_y;
        return direction;
    }
    updateFlightControlToggles();
    if (m_window.keyPressed(core::platform::glfw::WindowKey::Escape)) {
        m_running = false;
    }
    MovementIntent const intent{
        .strafe = static_cast<int8_t>(
            static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::D))
            - static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::A))
        ),
        .forward = static_cast<int8_t>(
            static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::W))
            - static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::S))
        ),
    };
    MovementDirection const movement = CameraController::cameraRelativeMovement(
        intent,
        m_look_camera.pose().angles.yaw_degrees
    );
    glm::dvec3 const view = m_look_camera.forward();
    int8_t const view_x = CameraController::quantize(view.x);
    int8_t const view_y = CameraController::quantize(view.y);
    bool const movement_capability_cycle = m_movement_capability_latch.update(
        functionKeyPressed(m_window.nativeHandle(), GLFW_KEY_F6, GLFW_KEY_6)
    );
    bool const jump_pressed = m_jump_queued
        || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_SPACE) == GLFW_PRESS;
    m_jump_queued = false;
    m_interest_heading_x = movement.x != 0 || movement.y != 0 ? movement.x : view_x;
    m_interest_heading_y = movement.x != 0 || movement.y != 0 ? movement.y : view_y;
    return shared::Direction{
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
        .z = static_cast<uint8_t>(CameraController::verticalMovement(
            jump_pressed,
            glfwGetKey(m_window.nativeHandle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
            || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS
        )),
        .accelerated = m_acceleration_enabled,
        .speedup = shared::FLIGHT_SPEEDUP_PROFILES[m_speedup_profile_index],
        .cycle_movement_capabilities = movement_capability_cycle,
        .view_x = view_x,
        .view_y = view_y,
    };
}

void PlayerClient::servicePlatformEvents() {
    m_window.pollEvents();
    if (m_window.shouldClose()) {
        m_running = false;
    }
}

void PlayerClient::render() {
    bool const capture_render_diagnostics = m_benchmark.has_value();
    bool const capture_mesh_diagnostics = m_benchmark.has_value() && m_benchmark->enable_mesh_stage_diagnostics;
    std::chrono::steady_clock::time_point const render_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics = {};
    }
    if (capture_mesh_diagnostics) {
        m_mesh_result_drain_diagnostics = {};
    }
    if (!m_window.nextFrame()) {
        m_running = false;
        return;
    }
    // Simulation is intentionally fixed at the 100 ms network tick. Preserve
    // a jump pressed between ticks so a short tap is consumed by the next
    // authoritative input instead of being lost to polling cadence.
    if (!m_capture.has_value()) {
        m_jump_queued = m_jump_queued
            || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_SPACE) == GLFW_PRESS;
        updateFlightControlToggles();
    }

    uint32_t width = 0U;
    uint32_t height = 0U;
    m_window.framebufferSize(width, height);
    if (m_capture.has_value() && !m_capture_label_initialized
        && m_capture_rotation_started
        && playerClientCaptureExtentMeetsMinimum(width, height)) {
        PlayerClientCapturePlan const plan = playerClientCapturePlan(m_capture->preset);
        static constexpr float MAX_LABEL_SCALE = 1.5F;
        static constexpr float LABEL_MARGIN = 24.0F;
        float const maximum_label_width = static_cast<float>(plan.title.size()) * 16.0F;
        float const available_label_width = static_cast<float>(width) - 2.0F * LABEL_MARGIN;
        float const label_scale = std::min(MAX_LABEL_SCALE, available_label_width / maximum_label_width);
        float const label_width = maximum_label_width * label_scale;
        float const label_x = std::max(LABEL_MARGIN, (static_cast<float>(width) - label_width) * 0.5F);
        float const label_y = std::max(
            LABEL_MARGIN,
            static_cast<float>(height) - 32.0F * label_scale - LABEL_MARGIN
        );
        m_renderer.addGuiText(plan.title, {
            .position = { label_x, label_y, 0.0F },
            .scale = label_scale,
        }, {
            .red = 1.0F, .green = 0.82F, .blue = 0.38F, .alpha = 1.0F,
        });
        m_capture_label_initialized = true;
    }
    if (!m_capture.has_value() && !(m_benchmark && m_benchmark->freeze_camera)) {
        double cursor_x = 0.0;
        double cursor_y = 0.0;
        glfwGetCursorPos(m_window.nativeHandle(), &cursor_x, &cursor_y);
        if (m_has_cursor_position) {
            static_cast<void>(m_look_camera.rotate(
                (cursor_x - m_last_cursor_x) * 0.15,
                (m_last_cursor_y - cursor_y) * 0.15
            ));
        }
        m_last_cursor_x = cursor_x;
        m_last_cursor_y = cursor_y;
        m_has_cursor_position = true;
        bool const camera_perspective_pressed = functionKeyPressed(
            m_window.nativeHandle(), GLFW_KEY_F5, GLFW_KEY_5
        );
        if (m_camera_perspective_latch.update(camera_perspective_pressed)) {
            m_camera_perspective = nextCameraPerspective(m_camera_perspective);
        }
    } else if (m_capture.has_value()) {
        CameraAngles angles = playerClientCapturePlan(m_capture->preset).look_angles;
        angles.yaw_degrees += 360.0 / CAPTURE_ROTATION_SECTORS * m_capture_rotation_step;
        static_cast<void>(m_look_camera.setAngles(angles));
    }
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    if (capture_render_diagnostics) {
        m_render_diagnostics.window_input_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - render_started_at);
    }
    if (m_capture.has_value()) {
        ++m_capture_frames;
    }
    std::chrono::steady_clock::time_point const renderer_deadline = now
        + playerClientRendererFrameBudget(m_capture.has_value());
    shared::HeightTileSurfaceProjection const mesh_projection{
        .vertical_fov_degrees = m_camera.projection().vertical_fov_degrees,
        .viewport_width_pixels = width > 0U ? width : m_preview_mesh_projection.viewport_width_pixels,
        .viewport_height_pixels = height > 0U ? height : m_preview_mesh_projection.viewport_height_pixels,
    };
    std::chrono::steady_clock::time_point const interest_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (auto const player = m_world.playerByCharacter(m_local_character)) {
        refreshHeightTileInterest(*player, mesh_projection, capture_render_diagnostics);
    }
    std::chrono::steady_clock::time_point const interest_completed_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const mesh_work_deadline =
        playerClientPreviewMeshWorkDeadline(renderer_deadline, interest_completed_at);
    if (capture_render_diagnostics) {
        m_render_diagnostics.height_tile_interest_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(interest_completed_at - interest_started_at);
    }
    std::chrono::steady_clock::time_point const first_mesh_pump_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    processPendingPreviewMeshes(
        playerClientPreviewMeshJobBudget(m_capture.has_value()),
        mesh_work_deadline
    );
    std::chrono::steady_clock::time_point const first_mesh_pump_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.preview_mesh_pump_before_changes_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                first_mesh_pump_completed_at - first_mesh_pump_started_at);
    }
    std::chrono::steady_clock::time_point const tile_changes_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (HeightTileChange const change : heightTileResidency().takeChanges(
             playerClientHeightTileChangeBudget(m_capture.has_value()))) {
        if (m_capture.has_value()) {
            ++m_capture_height_changes;
        }
        if (change.kind == HeightTileChangeKind::Remove) {
            queuePreviewRemoval(change.key);
        } else {
            queuePreviewMesh(change.key);
        }
        queuePreviewMesh(offsetHeightTileKey(change.key, -1, 0));
        queuePreviewMesh(offsetHeightTileKey(change.key, 1, 0));
        queuePreviewMesh(offsetHeightTileKey(change.key, 0, -1));
        queuePreviewMesh(offsetHeightTileKey(change.key, 0, 1));
    }
    std::chrono::steady_clock::time_point const tile_changes_queued_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.height_tile_change_queue_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(tile_changes_queued_at - tile_changes_started_at);
    }
    uint32_t const removal_budget = m_pending_preview_removals.empty()
        ? 0U
        : playerClientPreviewRemovalBudget();
    m_preview_mesh_removal_batch.clear();
    if (m_preview_mesh_removal_batch.capacity() < removal_budget) {
        m_preview_mesh_removal_batch.reserve(removal_budget);
    }
    std::chrono::steady_clock::time_point const removal_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (uint32_t removed = 0U;
         removed < removal_budget
         && !m_pending_preview_removals.empty();
         ++removed) {
        HeightTileKey const key = m_pending_preview_removals.front();
        m_pending_preview_removals.pop_front();
        m_pending_preview_removal_set.erase(key);
        if (playerClientShouldRemovePreviewMesh(m_height_tile_interest.contains(key))) {
            removePreviewMesh(key, m_preview_mesh_removal_batch);
        }
    }
    std::chrono::steady_clock::time_point const renderer_remove_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // Empty diagnostic batches reset the renderer's per-frame removal sample.
    if (!m_preview_mesh_removal_batch.empty() || capture_render_diagnostics) {
        static_cast<void>(m_renderer.removeHeightTileMeshes(
            m_preview_mesh_removal_batch,
            capture_render_diagnostics
        ));
    }
    submitPreviewCacheRetirements();
    std::chrono::steady_clock::time_point const removal_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.preview_mesh_remove_duration +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(removal_completed_at - removal_started_at);
        m_render_diagnostics.preview_mesh_remove_renderer_duration +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(removal_completed_at - renderer_remove_started_at);
        m_render_diagnostics.preview_mesh_remove_count += static_cast<uint32_t>(m_preview_mesh_removal_batch.size());
    }
    std::chrono::steady_clock::time_point const tile_changes_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.height_tile_change_and_removal_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(tile_changes_completed_at - tile_changes_started_at);
    }
    std::chrono::steady_clock::time_point const second_mesh_pump_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    processPendingPreviewMeshes(
        playerClientPreviewMeshJobBudget(m_capture.has_value()),
        mesh_work_deadline
    );
    std::chrono::steady_clock::time_point const second_mesh_pump_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.preview_mesh_pump_after_changes_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                second_mesh_pump_completed_at - second_mesh_pump_started_at);
    }
    std::chrono::steady_clock::time_point const camera_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::optional<PlayerPresentationPosition> const local_position = predictedLocalPresentation(now);
    double camera_distance = MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
    if (local_position.has_value()) {
        camera_distance = maximumUnobstructedCameraDistance(*local_position);
        PlayerCameraView camera_view = resolveLocalPlayerCamera(
            *local_position,
            m_look_camera.pose().angles,
            m_camera_perspective,
            camera_distance
        );
        if (m_capture.has_value() && m_camera_perspective == CameraPerspective::ThirdPersonRear) {
            camera_view.pose.position = localPlayerEyePosition(*local_position)
                - m_look_camera.forward() * camera_distance;
        }
        static_cast<void>(m_camera.setPosition(camera_view.pose.position));
        static_cast<void>(m_camera.setAngles(camera_view.pose.angles));
    }
    std::chrono::steady_clock::time_point const camera_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.camera_resolution_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(camera_completed_at - camera_started_at);
    }
    std::chrono::steady_clock::time_point const resize_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.before_resize_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(resize_started_at - render_started_at);
    }
    m_renderer.recreate(width, height);
    std::chrono::steady_clock::time_point const resize_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.resize_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(resize_completed_at - resize_started_at);
    }
    m_render_data.clear();
    m_render_data.reserve(m_world.players().size());
    for (shared::Player const& p : m_world.players()) {
        if (!shouldRenderPlayerBody(p, m_local_character, m_camera_perspective)) {
            continue;
        }
        if (auto const position = m_player_presentation.sample(p.ch, now)) {
            m_render_data.push_back({
                .x = static_cast<float>(position->x),
                .y = static_cast<float>(position->y),
                .color = playerPaletteColor(p.palette_index),
                .z = static_cast<float>(position->z),
                .render_on_top = p.ch == m_local_character
                    && m_camera_perspective != CameraPerspective::FirstPerson
                    && camera_distance <= 0.0,
            });
        }
    }
    DebugHudInput const debug_hud_input = [&] {
        DebugHudInput input;
        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.player_x = static_cast<float>(shared::playerPositionX(*player));
            input.player_y = static_cast<float>(shared::playerPositionY(*player));
            input.player_z = static_cast<float>(shared::playerPositionZ(*player));
        }
        CameraAngles const angles = m_camera.pose().angles;
        input.camera_yaw_degrees = static_cast<float>(angles.yaw_degrees);
        input.camera_pitch_degrees = static_cast<float>(angles.pitch_degrees);
        input.camera_roll_degrees = static_cast<float>(angles.roll_degrees);
        input.speedup = m_acceleration_enabled
            ? shared::FLIGHT_SPEEDUP_PROFILES[m_speedup_profile_index]
            : 1U;
        input.selected_speedup = shared::FLIGHT_SPEEDUP_PROFILES[m_speedup_profile_index];
        input.acceleration_enabled = m_acceleration_enabled;
        input.view_index = static_cast<uint8_t>(m_camera_perspective);
        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.flight_enabled = player->movement_capabilities.allows(shared::MovementCapability::Flight);
            input.collision_bypass_enabled = player->movement_capabilities.allows(
                shared::MovementCapability::CollisionBypass
            );
        }
        return input;
    }();
    float content_scale_x = 1.0F;
    float content_scale_y = 1.0F;
    glfwGetWindowContentScale(m_window.nativeHandle(), &content_scale_x, &content_scale_y);
    bool const debug_hud_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_F1) == GLFW_PRESS;
    if (m_debug_hud_toggle.update(debug_hud_pressed)) {
        m_renderer.toggleDebugHud();
    }
    m_renderer.setCamera(m_camera.pose(), m_camera.projection());
    std::chrono::steady_clock::time_point const renderer_started_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.after_resize_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(renderer_started_at - resize_completed_at);
    }
    m_last_presentation_succeeded = m_renderer.render(
        m_render_data,
        debug_hud_input,
        std::max(content_scale_x, content_scale_y),
        renderer_deadline
    );
    std::chrono::steady_clock::time_point const renderer_completed_at =
        capture_render_diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (capture_render_diagnostics) {
        m_render_diagnostics.renderer_call_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(renderer_completed_at - renderer_started_at);
    }

    if (m_capture.has_value()) {
        if (!m_capture_started_at.has_value()) {
            m_capture_started_at = now;
        }
        if (!m_capture_failed && !m_capture_succeeded
            && m_capture_rotation_started_at.has_value()
            && now - *m_capture_rotation_started_at > m_capture->sweep_deadline) {
            failCapture("heading sweep or framebuffer readback deadline exceeded");
            return;
        }
        if (!m_capture_failed && !m_capture_succeeded && !m_capture_requested) {
            std::optional<shared::Player> const player = predictedLocalPlayer();
            bool const at_target = player.has_value() && captureAtTarget(*player);
            bool const ready = at_target && captureSceneReady();
            glm::dvec3 const view = m_look_camera.forward();
            shared::HeightTileHeading const expected_heading = shared::canonicalHeightTileHeading(
                CameraController::quantize(view.x), CameraController::quantize(view.y)
            );
            bool const heading_applied = m_applied_interest_heading_x == expected_heading.x
                && m_applied_interest_heading_y == expected_heading.y;
            if (!m_capture_last_progress_at.has_value()
                || now - *m_capture_last_progress_at >= std::chrono::seconds{30}) {
                CORE_INFO("Capture progress: preset={} elapsed={}s target={} ready={} "
                          "heading_applied={} "
                          "heading_step={}/{} interest={} resident={} visible={} "
                          "uploaded={} required={}",
                    playerClientCapturePlan(m_capture->preset).name,
                    std::chrono::duration_cast<std::chrono::seconds>(now - *m_capture_started_at).count(),
                    at_target, ready, heading_applied, m_capture_rotation_step, CAPTURE_ROTATION_SECTORS,
                    m_height_tile_interest.size(), heightTileResidency().stats().resident_tiles,
                    m_visible_preview_meshes.size(), m_renderer.runtimeInfo().height_tile_mesh_count,
                    requiredHeightTileCount());
                CORE_INFO("Capture mesh pipeline: pending={} jobs={} dirty={} "
                          "results_queued={} "
                          "results_drained={} publish_failures={} drain_ms={} "
                          "reprioritize_ms={}",
                    m_pending_preview_meshes.size(), m_preview_mesh_jobs.size(), m_preview_mesh_dirty_jobs.size(),
                    m_preview_mesh_workers->resultCount(), m_capture_mesh_results, m_capture_mesh_publish_failures,
                    m_capture_mesh_drain_micros / 1000U, m_capture_mesh_reprioritize_micros / 1000U);
                CORE_INFO("Capture intake: frames={} height_changes={} "
                          "height_change_backlog={}",
                    m_capture_frames, m_capture_height_changes, heightTileResidency().pendingChangeCount());
                if (!ready && m_height_tile_interest.size() == requiredHeightTileCount()
                    && heightTileResidency().stats().resident_tiles == requiredHeightTileCount()
                    && m_visible_preview_meshes.size() == requiredHeightTileCount()
                    && m_renderer.runtimeInfo().height_tile_mesh_count == requiredHeightTileCount()
                    && m_height_tile_interest.center().has_value()) {
                    size_t missing_visible = 0U;
                    size_t missing_resident = 0U;
                    HeightTileKey const center = *m_height_tile_interest.center();
                    int32_t const radius = static_cast<int32_t>(m_height_tile_interest.radius());
                    for (int32_t y = -radius; y <= radius; ++y) {
                        for (int32_t x = -radius; x <= radius; ++x) {
                            HeightTileKey const key = offsetHeightTileKey(center, x, y);
                            bool const desired = m_height_tile_interest.contains(key);
                            missing_visible += desired && !m_visible_preview_meshes.contains(key);
                            missing_resident += desired && !heightTileResidency().resident(key);
                        }
                    }
                    CORE_INFO("Capture exact-coverage diagnosis: pending_removals={} "
                              "dirty_jobs={} "
                              "missing_visible={} missing_resident={} pending_changes={}",
                        m_pending_preview_removals.size(), m_preview_mesh_dirty_jobs.size(), missing_visible,
                        missing_resident, heightTileResidency().pendingChangeCount());
                }
                m_capture_frames = 0U;
                m_capture_height_changes = 0U;
                m_capture_mesh_results = 0U;
                m_capture_mesh_publish_failures = 0U;
                m_capture_mesh_drain_micros = 0U;
                m_capture_mesh_reprioritize_micros = 0U;
                m_capture_last_progress_at = now;
            }
            if (!m_capture_rotation_started
                && now - *m_capture_started_at > m_capture->readiness_deadline) {
                CORE_ERROR("Capture readiness: target={} ready={} interest={} "
                           "resident={} visible={} "
                           "uploaded={} required={} pending_changes={} "
                           "pending_meshes={} jobs={} "
                           "pending_removals={} dirty_jobs={}",
                    at_target, ready, m_height_tile_interest.size(), heightTileResidency().stats().resident_tiles,
                    m_visible_preview_meshes.size(), m_renderer.runtimeInfo().height_tile_mesh_count,
                    requiredHeightTileCount(), heightTileResidency().pendingChangeCount(),
                    m_pending_preview_meshes.size(), m_preview_mesh_jobs.size(),
                    m_pending_preview_removals.size(), m_preview_mesh_dirty_jobs.size());
                failCapture("readiness deadline exceeded");
                return;
            }
            if (m_capture_rotation_started && !ready) {
                failCapture("rotation lost full player-centered uploaded mesh coverage");
                return;
            }
            if (ready && heading_applied) {
                if (!m_capture_rotation_started) {
                    m_capture_interest_baseline = m_height_tile_interest;
                    m_capture_mesh_baseline = m_visible_preview_meshes;
                    m_capture_rotation_started = true;
                    m_capture_rotation_started_at = now;
                    m_capture_rotation_step = 1U;
                    CORE_INFO("Capture sweep started: readiness_elapsed={}s step={}/{} "
                              "required={}",
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - *m_capture_started_at
                        ).count(), m_capture_rotation_step, CAPTURE_ROTATION_SECTORS,
                        requiredHeightTileCount());
                } else if (m_height_tile_interest != m_capture_interest_baseline
                    || m_visible_preview_meshes != m_capture_mesh_baseline) {
                    failCapture("camera heading changed resident or uploaded mesh keys");
                    return;
                } else if (m_capture_rotation_step >= CAPTURE_ROTATION_SECTORS) {
                    CORE_INFO("Capture sweep complete: headings={} elapsed={}s; "
                              "requesting framebuffer readback",
                        CAPTURE_ROTATION_SECTORS,
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - *m_capture_rotation_started_at
                        ).count());
                    m_renderer.requestFrameCapture();
                    m_capture_requested = true;
                } else {
                    ++m_capture_rotation_step;
                    CORE_INFO("Capture sweep heading: step={}/{}", m_capture_rotation_step,
                        CAPTURE_ROTATION_SECTORS);
                }
            }
        }
        if (std::optional<RendererFrameCapture> capture = m_renderer.takeFrameCapture(); capture.has_value()) {
            if (!playerClientCaptureExtentMeetsMinimum(capture->width, capture->height)) {
                std::string const reason = "framebuffer capture is below 1000x600 (actual "
                    + std::to_string(capture->width) + "x" + std::to_string(capture->height) + ")";
                failCapture(reason);
                return;
            }
            std::ofstream output{m_capture->image_path, std::ios::binary | std::ios::trunc};
            output << "P6\n" << capture->width << ' ' << capture->height << "\n255\n";
            for (size_t pixel = 0U; pixel < capture->rgba8.size(); pixel += 4U) {
                output.write(reinterpret_cast<char const*>(capture->rgba8.data() + pixel), 3);
            }
            if (!output) {
                failCapture("failed to write image");
                return;
            }
            m_capture_succeeded = true;
            m_running = false;
        }
    }

    bool const reload_pressed = m_window.keyPressed(core::platform::glfw::WindowKey::R);
    if (reload_pressed && !m_was_reload_pressed) {
        m_renderer.hotReload();
    }
    m_was_reload_pressed = reload_pressed;
    if (capture_render_diagnostics) {
        m_render_diagnostics.after_renderer_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - renderer_completed_at);
    }
}

double PlayerClient::maximumUnobstructedCameraDistance(
    PlayerPresentationPosition const& local_position
) const noexcept
{
    if (m_camera_perspective == CameraPerspective::FirstPerson) {
        return MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
    }

    double const maximum_distance = m_capture.has_value()
        && m_camera_perspective == CameraPerspective::ThirdPersonRear
        ? playerClientCapturePlan(m_capture->preset).rear_camera_distance
        : MAX_LOCAL_PLAYER_CAMERA_DISTANCE;

    glm::dvec3 const eye = localPlayerEyePosition(local_position);
    PlayerCameraView const intended_camera = resolveLocalPlayerCamera(
        local_position,
        m_look_camera.pose().angles,
        m_camera_perspective,
        MAX_LOCAL_PLAYER_CAMERA_DISTANCE
    );
    glm::dvec3 const intended_position = m_capture.has_value()
        && m_camera_perspective == CameraPerspective::ThirdPersonRear
        ? eye - m_look_camera.forward() * maximum_distance
        : intended_camera.pose.position;
    glm::dvec3 const ray = (intended_position - eye) / maximum_distance;
    auto const obstructed = [&](double const distance) {
        glm::dvec3 const probe = eye + ray * distance;
        int64_t const probe_z = static_cast<int64_t>(std::floor(probe.z));
        if (!shared::WorldBounds::isValidZ(probe_z)) {
            return false;
        }
        uint32_t const world_x = shared::WorldBounds::wrapHorizontal(static_cast<int64_t>(std::floor(probe.x)));
        uint32_t const world_y = shared::WorldBounds::wrapHorizontal(static_cast<int64_t>(std::floor(probe.y)));
        HeightTileKey const key = shared::normalizeHeightTileKey({
            .x = static_cast<int32_t>(world_x / shared::HEIGHT_TILE_SIDE_LENGTH),
            .y = static_cast<int32_t>(world_y / shared::HEIGHT_TILE_SIDE_LENGTH),
        });
        HeightTileHandle const tile = heightTileResidency().resident(key);
        if (!tile) {
            return false;
        }
        uint32_t const tile_x = world_x % shared::HEIGHT_TILE_SIDE_LENGTH;
        uint32_t const tile_y = world_y % shared::HEIGHT_TILE_SIDE_LENGTH;
        uint16_t const terrain_height = tile->heights()[tile_y * shared::HEIGHT_TILE_SIDE_LENGTH + tile_x];
        return probe_z < static_cast<int64_t>(terrain_height);
    };
    if (obstructed(0.0)) {
        return 0.0;
    }
    return ::client::maximumUnobstructedCameraDistance(
        maximum_distance,
        CAMERA_OBSTRUCTION_SAMPLE_COUNT,
        CAMERA_OBSTRUCTION_BINARY_STEPS,
        CAMERA_OBSTRUCTION_MARGIN,
        obstructed
    );
}

} // namespace client
