#include <client/PlayerClient.hpp>

#include <client/CameraController.hpp>
#include <client/CameraObstruction.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewMeshing.hpp>

#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/World.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/Log.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
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
            CameraPerspective::ThirdPersonRear, { .yaw_degrees = 110.0, .pitch_degrees = 8.0 },
            { .x = 32'470, .y = 32'768, .z = 480 }, std::nullopt, 90.0, 12.0 };
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

uint32_t playerClientHeightTileChangeBudget(bool const capture_enabled) noexcept
{
    return capture_enabled ? 128U : 16U;
}

uint32_t playerClientPreviewMeshJobBudget(bool const capture_enabled) noexcept
{
    return capture_enabled ? 32U : 16U;
}

bool playerClientShouldQueuePreviewRemoval(bool const capture_enabled, bool const mesh_visible) noexcept
{
    return !capture_enabled || mesh_visible;
}

uint32_t playerClientPreviewRemovalBudget(bool const capture_enabled, bool const current_coverage) noexcept
{
    return capture_enabled ? 128U : (current_coverage ? 16U : 0U);
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

[[nodiscard]] int8_t captureAxisComponent(int64_t const delta, uint16_t const speedup) noexcept
{
    if (delta == 0) {
        return 0;
    }
    uint16_t const effective_speedup = speedup == CAPTURE_NEAR_SPEEDUP ? 1U : speedup;
    int64_t const maximum_step = static_cast<int64_t>(shared::MOVEMENT_SUBCELLS_PER_TICK)
        * effective_speedup;
    int64_t const magnitude = std::clamp(absoluteValue(delta) * 127 / maximum_step, int64_t{1}, int64_t{127});
    return static_cast<int8_t>(delta < 0 ? -magnitude : magnitude);
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

class PlayerClient::PreviewMeshWorkerPool final {
public:
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

    explicit PreviewMeshWorkerPool(size_t const maximum_queued_meshes)
        : m_maximum_queued_meshes(maximum_queued_meshes)
    {
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
    }

    [[nodiscard]] bool enqueue(Job job)
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_stopping || m_outstanding_meshes >= m_maximum_queued_meshes) {
                return false;
            }
            m_work.push_back(std::move(job));
            ++m_outstanding_meshes;
        }
        m_work_available.notify_one();
        return true;
    }

    void setPriority(HeightTileKey const center, int8_t const heading_x, int8_t const heading_y)
    {
        std::lock_guard lock{m_mutex};
        m_priority_center = center;
        m_priority_heading_x = heading_x;
        m_priority_heading_y = heading_y;
    }

    [[nodiscard]] std::vector<HeightTileKey> cancelQueuedOutsideInterest(
        std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& interest
    )
    {
        std::vector<HeightTileKey> cancelled;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_work, [&](Job const& job) {
            if (interest.contains(job.key)) {
                return false;
            }
            cancelled.push_back(job.key);
            --m_outstanding_meshes;
            return true;
        });
        return cancelled;
    }

    [[nodiscard]] bool canAccept() const
    {
        std::lock_guard lock{m_mutex};
        return !m_stopping && m_outstanding_meshes < m_maximum_queued_meshes;
    }

    [[nodiscard]] size_t resultCount() const
    {
        std::lock_guard lock{m_mutex};
        return m_results.size();
    }

    [[nodiscard]] std::optional<Result> takeResult()
    {
        std::lock_guard lock{m_mutex};
        if (m_results.empty()) {
            return std::nullopt;
        }
        Result result = std::move(m_results.front());
        m_results.pop_front();
        --m_outstanding_meshes;
        return result;
    }

private:
    void workerLoop()
    {
        auto const priority = [this](HeightTileKey const key) {
            return std::tuple{
                shared::heightTileInterestPriority(
                    m_priority_center,
                    m_priority_heading_x,
                    m_priority_heading_y,
                    key
                ),
                key.y,
                key.x,
            };
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
            m_results.push_back(std::move(result));
        }
    }

    size_t const m_maximum_queued_meshes;
    mutable std::mutex m_mutex;
    std::condition_variable m_work_available;
    std::deque<Job> m_work;
    std::deque<Result> m_results;
    std::vector<std::thread> m_workers;
    size_t m_outstanding_meshes = 0U;
    HeightTileKey m_priority_center{};
    int8_t m_priority_heading_x = 0;
    int8_t m_priority_heading_y = 127;
    bool m_stopping = false;
};

PlayerClient::~PlayerClient()
{
    if (m_window.nativeHandle() != nullptr) {
        glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

void PlayerClient::onConnectionStateReset()
{
    m_jump_queued = false;
    ++m_preview_mesh_epoch;
    for (HeightTileKey const key : m_visible_preview_meshes) {
        static_cast<void>(m_renderer.removeHeightTileMesh({ .x = key.x, .y = key.y }));
    }
    m_visible_preview_meshes.clear();
    m_preview_mesh_coverage.clear();
    m_visible_preview_mesh_tiles.clear();
    m_visible_preview_mesh_bases.clear();
    m_visible_preview_mesh_elevation_ranges.clear();
    m_visible_preview_mesh_seam_bridges.clear();
    m_preview_lod.clear();
    m_preview_tile_elevations.clear();
    m_height_tile_center.reset();
    m_height_tile_interest.clear();
    m_pending_preview_removals.clear();
    m_pending_preview_removal_set.clear();
    m_pending_preview_meshes.clear();
    m_preview_mesh_dirty_jobs.clear();
    for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(m_height_tile_interest)) {
        m_preview_mesh_jobs.erase(key);
    }
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

PlayerClient::PlayerClient(
    shared::WorldMode const mode,
    std::optional<PlayerClientCaptureOptions> capture,
    std::optional<PlayerClientBenchmarkOptions> benchmark
)
    : GameClient{ mode }
    , m_capture(std::move(capture))
    , m_benchmark(std::move(benchmark))
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
    , m_preview_mesh_workers(std::make_unique<PreviewMeshWorkerPool>(
        playerClientPreviewMeshJobBudget(m_capture.has_value())
    ))
{
    beginContinuousLook();
    static_cast<void>(m_camera.setProjection({
        .far_plane = static_cast<double>(shared::HEIGHT_TILE_INTEREST_RADIUS)
                * shared::HEIGHT_TILE_SIDE_LENGTH
            + shared::WorldExtent::DEPTH,
    }));
    m_renderer.setDebugHudEnabled(true);
    if (m_benchmark) {
        m_renderer.setDebugHudEnabled(false);
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

void PlayerPreviewMeshCoverage::interestRemoved(bool const visible) noexcept
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

void PlayerPreviewMeshQueue::clear() noexcept
{
    m_heap.clear();
    m_unranked.clear();
    m_keys.clear();
}

bool PlayerPreviewMeshQueue::push(HeightTileKey const key)
{
    if (!m_keys.insert(key).second) {
        return false;
    }
    m_heap.push_back(entryFor(key));
    std::push_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    return true;
}

void PlayerPreviewMeshQueue::resetPriority(
    HeightTileKey const center,
    int8_t const heading_x,
    int8_t const heading_y,
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& interest
)
{
    for (auto& heap : m_unranked) {
        m_heap.insert(m_heap.end(), heap.begin(), heap.end());
    }
    m_unranked.clear();
    m_center = center;
    m_heading_x = heading_x;
    m_heading_y = heading_y;
    std::erase_if(m_heap, [this, &interest](Entry const& entry) {
        if (interest.contains(entry.key)) {
            return false;
        }
        m_keys.erase(entry.key);
        return true;
    });
    for (Entry& entry : m_heap) {
        entry.priority = entryFor(entry.key).priority;
    }
    std::make_heap(m_heap.begin(), m_heap.end(), lowerPriority);
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
        m_heap = {};
    }
}

uint32_t PlayerPreviewMeshQueue::refreshPriority(
    uint32_t const maximum_entries,
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const& interest
)
{
    uint32_t refreshed = 0U;
    while (refreshed < maximum_entries && !m_unranked.empty()) {
        std::vector<Entry>& heap = m_unranked.front();
        std::pop_heap(heap.begin(), heap.end(), lowerPriority);
        HeightTileKey const key = heap.back().key;
        heap.pop_back();
        if (heap.empty()) {
            m_unranked.pop_front();
        }
        ++refreshed;
        if (!interest.contains(key)) {
            m_keys.erase(key);
            continue;
        }
        m_heap.push_back(entryFor(key));
        std::push_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    }
    return refreshed;
}

bool PlayerPreviewMeshQueue::empty() const noexcept
{
    return m_keys.empty();
}

bool PlayerPreviewMeshQueue::hasReady() const noexcept
{
    return !m_heap.empty();
}

size_t PlayerPreviewMeshQueue::size() const noexcept
{
    return m_keys.size();
}

HeightTileKey PlayerPreviewMeshQueue::top() const noexcept
{
    return m_heap.front().key;
}

void PlayerPreviewMeshQueue::pop()
{
    std::pop_heap(m_heap.begin(), m_heap.end(), lowerPriority);
    m_keys.erase(m_heap.back().key);
    m_heap.pop_back();
}

PlayerPreviewMeshQueue::Entry PlayerPreviewMeshQueue::entryFor(HeightTileKey const key) const noexcept
{
    return {
        .priority = shared::heightTileInterestPriority(m_center, m_heading_x, m_heading_y, key),
        .key = key,
    };
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

void PlayerClient::refreshHeightTileInterest(
    shared::Player const& player,
    shared::HeightTileSurfaceProjection const projection
)
{
    shared::HeightTileKey const center = shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(player.x),
        .y = floorDivideByHeightTileSide(player.y),
    });
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
    bool const center_changed = !m_height_tile_center.has_value() || *m_height_tile_center != center;
    bool const heading_changed = m_applied_interest_heading_x != heading.x
        || m_applied_interest_heading_y != heading.y;
    if (center_changed) {
        PlayerPreviewInterestDelta delta;
        if (m_height_tile_center) {
            delta = playerPreviewInterestDelta(*m_height_tile_center, center);
        } else {
            delta.additions = shared::makeHeightTileInterest(center, heading.x, heading.y).keys;
            m_height_tile_interest.reserve(delta.additions.size());
        }
        for (HeightTileKey const key : delta.removals) {
            m_preview_mesh_coverage.interestRemoved(m_visible_preview_meshes.contains(key));
            m_height_tile_interest.erase(key);
            m_preview_lod.erase(key);
            m_preview_tile_elevations.erase(key);
            queuePreviewRemoval(key);
        }
        for (HeightTileKey const key : delta.additions) {
            if (m_height_tile_interest.insert(key).second) {
                m_preview_mesh_coverage.interestAdded(m_visible_preview_meshes.contains(key));
                queuePreviewMesh(key);
            }
        }
    }
    m_height_tile_center = center;
    m_applied_interest_heading_x = heading.x;
    m_applied_interest_heading_y = heading.y;
    m_preview_mesh_projection = projection;
    if (center_changed || heading_changed) {
        auto const priority_started = std::chrono::steady_clock::now();
        if (m_capture) {
            m_pending_preview_meshes.resetPriority(center, heading.x, heading.y, m_height_tile_interest);
        } else {
            m_pending_preview_meshes.beginPriorityRefresh(center, heading.x, heading.y);
        }
        if (m_capture.has_value()) {
            m_capture_mesh_reprioritize_micros += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - priority_started
                ).count()
            );
        }
        m_preview_mesh_workers->setPriority(center, heading.x, heading.y);
        if (center_changed) {
            for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(
                     m_height_tile_interest
                 )) {
                m_preview_mesh_jobs.erase(key);
                m_preview_mesh_dirty_jobs.erase(key);
            }
        }
    }
    PlayerPreviewLodRefresh const refresh = m_preview_lod.refresh(center, viewer_bounds, projection);
    for (HeightTileKey const key : refresh.changed_keys) {
        queuePreviewMesh(key);
        for (HeightTileKey const adjacent : {
                 offsetHeightTileKey(key, -1, 0),
                 offsetHeightTileKey(key, 1, 0),
                 offsetHeightTileKey(key, 0, -1),
                 offsetHeightTileKey(key, 0, 1),
             }) {
            queuePreviewMesh(adjacent);
        }
    }
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
)
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
            details[index] = previewMeshDetail(neighbor_keys[index], neighbors[index]);
        }
    }
    return details;
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
            m_visible_preview_mesh_seam_bridges.insert_or_assign(key, *candidate_bridges);
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

void PlayerClient::publishPreviewMesh(
    HeightTileKey const key,
    shared::HeightTileSurfaceMesh mesh,
    HeightTileHandle const& tile,
    std::chrono::steady_clock::time_point const deadline
)
{
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
    std::array<shared::HeightTileSurfaceElevationRange, 4> elevation_ranges;
    for (uint32_t edge = 0U; edge < elevation_ranges.size(); ++edge) {
        elevation_ranges[edge] = shared::heightTileSurfaceEdgeElevationRange(
            mesh, PreviewMeshSeamBridgeSet::EDGES[edge]
        );
    }
    bool const published = publishPreviewMeshSeamPlan(
        plan,
        batch,
        [this, deadline](std::span<shared::HeightTileSurfaceMesh const> candidates) {
            return m_renderer.upsertHeightTileMeshes(candidates, deadline);
        },
        [this, key, &mesh, &tile, &adjacent_keys, &elevation_ranges](
            PreviewMeshSeamPublicationPlan& accepted_plan
        ) {
            markPreviewMeshVisible(key);
            m_visible_preview_mesh_bases.insert_or_assign(key, std::move(mesh));
            m_visible_preview_mesh_elevation_ranges.insert_or_assign(key, elevation_ranges);
            m_visible_preview_mesh_seam_bridges.insert_or_assign(key, std::move(accepted_plan.center_bridges));
            m_visible_preview_mesh_tiles.insert_or_assign(key, tile);
            for (uint32_t index = 0U; index < accepted_plan.changed_neighbors.size(); ++index) {
                if (accepted_plan.changed_neighbors[index]) {
                    markPreviewMeshVisible(adjacent_keys[index]);
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
        queuePreviewMesh(key);
    }
}

void PlayerClient::removePreviewMesh(
    HeightTileKey const key,
    std::chrono::steady_clock::time_point const deadline
)
{
    static_cast<void>(m_renderer.removeHeightTileMesh({ .x = key.x, .y = key.y }));
    m_visible_preview_meshes.erase(key);
    m_visible_preview_mesh_tiles.erase(key);
    m_visible_preview_mesh_bases.erase(key);
    m_visible_preview_mesh_elevation_ranges.erase(key);
    m_visible_preview_mesh_seam_bridges.erase(key);
    refreshPreviewMeshNeighborElevations(key);
    uint32_t edge = 0U;
    for (HeightTileKey const adjacent : {
             offsetHeightTileKey(key, -1, 0),
             offsetHeightTileKey(key, 1, 0),
             offsetHeightTileKey(key, 0, -1),
             offsetHeightTileKey(key, 0, 1),
         }) {
        auto const seams = m_visible_preview_mesh_seam_bridges.find(adjacent);
        if (seams != m_visible_preview_mesh_seam_bridges.end()) {
            PreviewMeshSeamBridgeSet candidate_bridges = seams->second;
            if (candidate_bridges.replace(PreviewMeshSeamBridgeSet::OPPOSITE_EDGES[edge], {})) {
                static_cast<void>(refreshVisiblePreviewMesh(adjacent, deadline, &candidate_bridges));
            }
        }
        ++edge;
    }
}

shared::HeightTileSurfaceDetail PlayerClient::previewMeshDetail(
    HeightTileKey const key,
    HeightTileHandle const& tile
)
{
    if (!tile || !m_height_tile_center.has_value()) {
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
    std::array<std::optional<shared::HeightTileSurfaceElevationRange>, 4> neighbor_mesh_ranges{};
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        if (neighbors[index]) {
            neighbor_tokens[index] = neighbors[index]->token();
        }
        auto const installed = m_visible_preview_mesh_elevation_ranges.find(neighbor_keys[index]);
        if (installed != m_visible_preview_mesh_elevation_ranges.end()) {
            neighbor_mesh_ranges[index] = installed->second[PreviewMeshSeamBridgeSet::OPPOSITE_EDGES[index]];
        }
    }
    auto elevation = m_preview_tile_elevations.find(key);
    if (elevation == m_preview_tile_elevations.end()
        || elevation->second.tile_revision != tile->revision()
        || elevation->second.tile_token != tile->token()
        || elevation->second.neighbor_tokens != neighbor_tokens
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
            if (neighbor_mesh_ranges[index]) {
                includeHeight(neighbor_mesh_ranges[index]->minimum);
                includeHeight(neighbor_mesh_ranges[index]->maximum);
            }
        }
        elevation = m_preview_tile_elevations.insert_or_assign(key, PreviewTileElevation{
            .tile_revision = tile->revision(),
            .tile_token = tile->token(),
            .neighbor_tokens = neighbor_tokens,
            .neighbor_mesh_ranges = neighbor_mesh_ranges,
            .minimum = minimum,
            .maximum = maximum,
        }).first;
    }
    return m_preview_lod.selectDetail(key, elevation->second.minimum, elevation->second.maximum);
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
    if (!m_pending_preview_meshes.empty()
        || !m_preview_mesh_jobs.empty()
        || !m_preview_mesh_dirty_jobs.empty()) {
        return false;
    }
    return playerClientCaptureCoverageComplete(
        m_height_tile_interest,
        m_visible_preview_meshes,
        heightTileResidency(),
        shared::HEIGHT_TILE_INTEREST_COUNT,
        m_pending_preview_removals.size()
    );
}

bool PlayerClient::hasExactCurrentRendererMeshCoverage() const
{
    return hasExactCurrentPreviewMeshCoverage()
        && m_renderer.runtimeInfo().height_tile_mesh_count == shared::HEIGHT_TILE_INTEREST_COUNT;
}

void PlayerClient::processPendingPreviewMeshes(
    uint32_t const maximum_meshes,
    std::chrono::steady_clock::time_point const deadline
)
{
    if (!m_height_tile_center.has_value()) {
        while (m_preview_mesh_workers->takeResult().has_value()) {
        }
        return;
    }
    static constexpr uint32_t PRIORITY_REFRESH_BUDGET = 256U;
    static_cast<void>(m_pending_preview_meshes.refreshPriority(PRIORITY_REFRESH_BUDGET, m_height_tile_interest));
    auto const upload_started = std::chrono::steady_clock::now();
    for (uint32_t completed = 0U; completed < maximum_meshes; ++completed) {
        std::optional<PreviewMeshWorkerPool::Result> result = m_preview_mesh_workers->takeResult();
        if (!result.has_value()) {
            break;
        }
        if (m_capture.has_value()) {
            ++m_capture_mesh_results;
        }
        if (result->epoch != m_preview_mesh_epoch) {
            continue;
        }
        m_preview_mesh_jobs.erase(result->key);
        HeightTileHandle const current = heightTileResidency().resident(result->key);
        bool source_current = false;
        bool detail_current = false;
        if (m_height_tile_interest.contains(result->key) && current) {
            source_current = current == result->source.tile
                && previewMeshNeighbors(result->key) == result->source.neighbors
                && previewMeshNeighborDetails(result->key) == result->source.neighbor_details;
            shared::HeightTileSurfaceDetail const expected_mesh_detail = result->stage
                    == PreviewMeshStage::Coarse
                ? previewMeshCoarseDetail(result->target_detail)
                : result->target_detail;
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
        if (source_current && detail_current && can_publish) {
            publishPreviewMesh(
                result->key,
                std::move(result->mesh),
                result->source.tile,
                deadline
            );
            if (result->stage == PreviewMeshStage::Coarse
                && m_visible_preview_mesh_tiles.contains(result->key)) {
                queuePreviewMesh(result->key);
            }
        } else if (source_current && detail_current && !can_publish
            && result->stage == PreviewMeshStage::Coarse) {
            queuePreviewMesh(result->key);
        }
        if (m_preview_mesh_dirty_jobs.erase(result->key) > 0U || !source_current || !detail_current) {
            queuePreviewMesh(result->key);
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
    if (!m_preview_mesh_workers->canAccept()) {
        return;
    }
    while (m_pending_preview_meshes.hasReady()) {
        HeightTileKey const key = m_pending_preview_meshes.top();
        HeightTileHandle const tile = heightTileResidency().resident(key);
        if (!m_height_tile_interest.contains(key) || !tile) {
            m_pending_preview_meshes.pop();
            continue;
        }
        if (m_preview_mesh_jobs.contains(key)) {
            m_pending_preview_meshes.pop();
            continue;
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
        if (!m_preview_mesh_workers->enqueue(std::move(job))) {
            break;
        }
        m_preview_mesh_jobs.insert(key);
        m_pending_preview_meshes.pop();
    }
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
    int8_t const x = captureAxisComponent(requested_x, speedup);
    int8_t const y = captureAxisComponent(requested_y, speedup);
    int8_t const z = captureAxisComponent(requested_z, speedup);
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

void PlayerClient::failCapture(std::string_view const reason) noexcept
{
    if (!m_capture_failed) {
        PlayerClientCapturePlan const plan = playerClientCapturePlan(
            m_capture.value_or(PlayerClientCaptureOptions{}).preset
        );
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

void PlayerClient::render() {
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
    if (m_capture.has_value() && !m_capture_label_initialized && width == 2560U && height == 1440U) {
        PlayerClientCapturePlan const plan = playerClientCapturePlan(m_capture->preset);
        static constexpr float LABEL_SCALE = 1.5F;
        static constexpr float LABEL_MARGIN = 24.0F;
        float const label_width = static_cast<float>(plan.title.size()) * 16.0F * LABEL_SCALE;
        float const label_x = std::max(LABEL_MARGIN, (static_cast<float>(width) - label_width) * 0.5F);
        float const label_y = std::max(LABEL_MARGIN, static_cast<float>(height) - 32.0F * LABEL_SCALE - LABEL_MARGIN);
        m_renderer.addGuiText(plan.title, {
            .position = { label_x, label_y, 0.0F },
            .scale = LABEL_SCALE,
        }, {
            .red = 1.0F, .green = 0.82F, .blue = 0.38F, .alpha = 1.0F,
        });
        m_capture_label_initialized = true;
    }
    if (!m_capture.has_value()) {
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
    } else {
        CameraAngles angles = playerClientCapturePlan(m_capture->preset).look_angles;
        angles.yaw_degrees += 360.0 / CAPTURE_ROTATION_SECTORS * m_capture_rotation_step;
        static_cast<void>(m_look_camera.setAngles(angles));
    }
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
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
    if (auto const player = m_world.playerByCharacter(m_local_character)) {
        refreshHeightTileInterest(*player, mesh_projection);
    }
    processPendingPreviewMeshes(playerClientPreviewMeshJobBudget(m_capture.has_value()), renderer_deadline);
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
    uint32_t const removal_budget = m_pending_preview_removals.empty()
        ? 0U
        : playerClientPreviewRemovalBudget(
            m_capture.has_value(), m_capture.has_value() || hasCurrentPreviewMeshCoverage()
        );
    for (uint32_t removed = 0U;
         removed < removal_budget
         && !m_pending_preview_removals.empty();
         ++removed) {
        HeightTileKey const key = m_pending_preview_removals.front();
        m_pending_preview_removals.pop_front();
        m_pending_preview_removal_set.erase(key);
        if (!m_height_tile_interest.contains(key)) {
            removePreviewMesh(key, renderer_deadline);
        }
    }
    processPendingPreviewMeshes(playerClientPreviewMeshJobBudget(m_capture.has_value()), renderer_deadline);
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
    m_renderer.recreate(width, height);
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
    m_last_presentation_succeeded = m_renderer.render(
        m_render_data,
        debug_hud_input,
        std::max(content_scale_x, content_scale_y),
        renderer_deadline
    );

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
                CORE_INFO(
                    "Capture progress: preset={} elapsed={}s target={} ready={} heading_applied={} "
                    "heading_step={}/{} interest={} resident={} visible={} uploaded={} required={}",
                    playerClientCapturePlan(m_capture->preset).name,
                    std::chrono::duration_cast<std::chrono::seconds>(now - *m_capture_started_at).count(),
                    at_target, ready, heading_applied, m_capture_rotation_step, CAPTURE_ROTATION_SECTORS,
                    m_height_tile_interest.size(), heightTileResidency().stats().resident_tiles,
                    m_visible_preview_meshes.size(), m_renderer.runtimeInfo().height_tile_mesh_count,
                    m_capture->minimum_height_tile_meshes);
                CORE_INFO(
                    "Capture mesh pipeline: pending={} jobs={} dirty={} results_queued={} "
                    "results_drained={} publish_failures={} drain_ms={} reprioritize_ms={}",
                    m_pending_preview_meshes.size(), m_preview_mesh_jobs.size(), m_preview_mesh_dirty_jobs.size(),
                    m_preview_mesh_workers->resultCount(), m_capture_mesh_results, m_capture_mesh_publish_failures,
                    m_capture_mesh_drain_micros / 1000U, m_capture_mesh_reprioritize_micros / 1000U);
                CORE_INFO("Capture intake: frames={} height_changes={} height_change_backlog={}",
                    m_capture_frames, m_capture_height_changes, heightTileResidency().pendingChangeCount());
                if (!ready && m_height_tile_interest.size() == shared::HEIGHT_TILE_INTEREST_COUNT
                    && heightTileResidency().stats().resident_tiles == shared::HEIGHT_TILE_INTEREST_COUNT
                    && m_visible_preview_meshes.size() == shared::HEIGHT_TILE_INTEREST_COUNT
                    && m_renderer.runtimeInfo().height_tile_mesh_count == shared::HEIGHT_TILE_INTEREST_COUNT) {
                    size_t missing_visible = 0U;
                    size_t missing_resident = 0U;
                    for (HeightTileKey const key : m_height_tile_interest) {
                        missing_visible += !m_visible_preview_meshes.contains(key);
                        missing_resident += !heightTileResidency().resident(key);
                    }
                    CORE_INFO(
                        "Capture exact-coverage diagnosis: pending_removals={} dirty_jobs={} "
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
                CORE_ERROR(
                    "Capture readiness: target={} ready={} interest={} resident={} visible={} "
                    "uploaded={} required={} pending_changes={} pending_meshes={} jobs={} "
                    "pending_removals={} dirty_jobs={}",
                    at_target, ready, m_height_tile_interest.size(), heightTileResidency().stats().resident_tiles,
                    m_visible_preview_meshes.size(), m_renderer.runtimeInfo().height_tile_mesh_count,
                    m_capture->minimum_height_tile_meshes, heightTileResidency().pendingChangeCount(),
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
                    CORE_INFO("Capture sweep started: readiness_elapsed={}s step={}/{} required={}",
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - *m_capture_started_at
                        ).count(), m_capture_rotation_step, CAPTURE_ROTATION_SECTORS,
                        m_capture->minimum_height_tile_meshes);
                } else if (m_height_tile_interest != m_capture_interest_baseline
                    || m_visible_preview_meshes != m_capture_mesh_baseline) {
                    failCapture("camera heading changed resident or uploaded mesh keys");
                    return;
                } else if (m_capture_rotation_step >= CAPTURE_ROTATION_SECTORS) {
                    CORE_INFO("Capture sweep complete: headings={} elapsed={}s; requesting framebuffer readback",
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
            if (capture->width != 2560U || capture->height != 1440U) {
                failCapture("framebuffer capture is not 2560x1440");
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
