#include <client/PlayerClient.hpp>

#include <client/CameraController.hpp>
#include <client/CameraObstruction.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewMeshing.hpp>

#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>

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

namespace {

static constexpr uint32_t MAX_HEIGHT_TILE_CHANGES_PER_FRAME = 16U;
static constexpr uint32_t MAX_PREVIEW_MESHES_PER_FRAME = 16U;
constexpr uint32_t CAMERA_OBSTRUCTION_SAMPLE_COUNT = 24U;
constexpr uint32_t CAMERA_OBSTRUCTION_BINARY_STEPS = 8U;
constexpr double CAMERA_OBSTRUCTION_MARGIN = 0.03;

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

    PreviewMeshWorkerPool()
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
            if (m_stopping || m_outstanding_meshes >= MAX_QUEUED_MESHES) {
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
        return !m_stopping && m_outstanding_meshes < MAX_QUEUED_MESHES;
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

    static constexpr size_t MAX_QUEUED_MESHES = 16U;
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
    m_visible_preview_mesh_tiles.clear();
    m_visible_preview_mesh_bases.clear();
    m_visible_preview_mesh_seam_bridges.clear();
    m_preview_mesh_details.clear();
    m_preview_tile_elevations.clear();
    m_height_tile_center.reset();
    m_has_lod_viewer_bounds = false;
    m_height_tile_interest.clear();
    m_pending_preview_removals.clear();
    m_pending_preview_removal_set.clear();
    m_pending_preview_meshes.clear();
    m_pending_preview_mesh_set.clear();
    m_preview_mesh_dirty_jobs.clear();
    for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(m_height_tile_interest)) {
        m_preview_mesh_jobs.erase(key);
    }
    m_preview_mesh_jobs.clear();
    m_interest_heading_x = 0;
    m_interest_heading_y = 127;
    m_applied_interest_heading_x = 0;
    m_applied_interest_heading_y = 0;
    m_capture_pre_rotation_interest.reset();
    m_capture_rotation_frames = 0U;
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
    , m_preview_mesh_workers(std::make_unique<PreviewMeshWorkerPool>())
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
    return static_cast<size_t>((static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32U)
        ^ static_cast<uint32_t>(key.y));
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
    bool const lod_inputs_changed = !m_has_lod_viewer_bounds
        || m_preview_mesh_projection != projection
        || m_lod_viewer_bounds != viewer_bounds;
    if (!center_changed && !heading_changed && !lod_inputs_changed) {
        return;
    }

    bool membership_changed = false;
    if (center_changed || heading_changed) {
        shared::HeightTileInterest const next = shared::makeHeightTileInterest(
            center, heading.x, heading.y
        );
        std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> const next_interest{
            next.keys.begin(), next.keys.end()
        };
        membership_changed = m_height_tile_interest != next_interest;
        std::vector<HeightTileKey> departed;
        departed.reserve(m_height_tile_interest.size());
        for (HeightTileKey const key : m_height_tile_interest) {
            if (!next_interest.contains(key)) {
                departed.push_back(key);
            }
        }
        for (HeightTileKey const key : departed) {
            m_height_tile_interest.erase(key);
            m_preview_mesh_details.erase(key);
            m_preview_tile_elevations.erase(key);
            queuePreviewRemoval(key);
        }
        for (HeightTileKey const key : next.keys) {
            if (m_height_tile_interest.insert(key).second) {
                queuePreviewMesh(key);
            }
        }
    }
    m_height_tile_center = center;
    m_applied_interest_heading_x = heading.x;
    m_applied_interest_heading_y = heading.y;
    m_preview_mesh_projection = projection;
    m_lod_viewer_bounds = viewer_bounds;
    m_has_lod_viewer_bounds = true;
    std::erase_if(m_pending_preview_meshes, [this](HeightTileKey const key) {
        if (m_height_tile_interest.contains(key)) {
            return false;
        }
        m_pending_preview_mesh_set.erase(key);
        return true;
    });
    if (center_changed || heading_changed) {
        m_preview_mesh_workers->setPriority(center, heading.x, heading.y);
        if (center_changed || membership_changed) {
            for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(
                     m_height_tile_interest
                 )) {
                m_preview_mesh_jobs.erase(key);
                m_preview_mesh_dirty_jobs.erase(key);
            }
        }
    }
    if (center_changed || lod_inputs_changed || membership_changed) {
        for (HeightTileKey const key : m_height_tile_interest) {
            HeightTileHandle const tile = heightTileResidency().resident(key);
            if (!tile) {
                continue;
            }
            auto const previous = m_preview_mesh_details.find(key);
            std::optional<shared::HeightTileSurfaceDetail> const previous_detail = previous
                    == m_preview_mesh_details.end()
                ? std::nullopt
                : std::optional<shared::HeightTileSurfaceDetail>{ previous->second };
            shared::HeightTileSurfaceDetail const detail = previewMeshDetail(key, tile);
            if (!previous_detail.has_value() || *previous_detail != detail) {
                queuePreviewMesh(key);
                std::array<HeightTileKey, 4> const adjacent_keys{
                    offsetHeightTileKey(key, -1, 0),
                    offsetHeightTileKey(key, 1, 0),
                    offsetHeightTileKey(key, 0, -1),
                    offsetHeightTileKey(key, 0, 1),
                };
                for (HeightTileKey const adjacent : adjacent_keys) {
                    queuePreviewMesh(adjacent);
                }
            }
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
        m_visible_preview_meshes.insert(key);
        if (candidate_bridges != nullptr) {
            m_visible_preview_mesh_seam_bridges.insert_or_assign(key, *candidate_bridges);
        }
        return true;
    } else {
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
    bool const published = publishPreviewMeshSeamPlan(
        plan,
        batch,
        [this, deadline](std::span<shared::HeightTileSurfaceMesh const> candidates) {
            return m_renderer.upsertHeightTileMeshes(candidates, deadline);
        },
        [this, key, &mesh, &tile, &adjacent_keys](PreviewMeshSeamPublicationPlan& accepted_plan) {
            m_visible_preview_meshes.insert(key);
            m_visible_preview_mesh_bases.insert_or_assign(key, std::move(mesh));
            m_visible_preview_mesh_seam_bridges.insert_or_assign(key, std::move(accepted_plan.center_bridges));
            m_visible_preview_mesh_tiles.insert_or_assign(key, tile);
            for (uint32_t index = 0U; index < accepted_plan.changed_neighbors.size(); ++index) {
                if (accepted_plan.changed_neighbors[index]) {
                    m_visible_preview_meshes.insert(adjacent_keys[index]);
                    m_visible_preview_mesh_seam_bridges.insert_or_assign(
                        adjacent_keys[index],
                        std::move(accepted_plan.neighbor_bridges[index])
                    );
                }
            }
        }
    );
    if (!published) {
        queuePreviewMesh(key);
    }
}

shared::HeightTileSurfaceDetail PlayerClient::previewMeshDetail(
    HeightTileKey const key,
    HeightTileHandle const& tile
)
{
    if (!tile || !m_height_tile_center.has_value() || !m_has_lod_viewer_bounds) {
        return shared::HeightTileSurfaceDetail::Fine;
    }
    std::array<HeightTileHandle, 4> const neighbors = previewMeshNeighbors(key);
    std::array<uint64_t, 4> neighbor_tokens{};
    for (size_t index = 0U; index < neighbors.size(); ++index) {
        if (neighbors[index]) {
            neighbor_tokens[index] = neighbors[index]->token();
        }
    }
    auto elevation = m_preview_tile_elevations.find(key);
    if (elevation == m_preview_tile_elevations.end()
        || elevation->second.tile_token != tile->token()
        || elevation->second.neighbor_tokens != neighbor_tokens) {
        uint16_t minimum = std::numeric_limits<uint16_t>::max();
        uint16_t maximum = 0U;
        auto const includeHeight = [&minimum, &maximum](uint16_t const height) {
            minimum = std::min(minimum, height);
            maximum = std::max(maximum, height);
        };
        for (uint16_t const height : tile->heights()) {
            includeHeight(height);
        }
        constexpr uint32_t SIDE = shared::HEIGHT_TILE_SIDE_LENGTH;
        for (uint32_t offset = 0U; offset < SIDE; ++offset) {
            if (neighbors[0]) {
                includeHeight(neighbors[0]->heights()[offset * SIDE + SIDE - 1U]);
            } else {
                includeHeight(0U);
            }
            if (neighbors[1]) {
                includeHeight(neighbors[1]->heights()[offset * SIDE]);
            } else {
                includeHeight(0U);
            }
            if (neighbors[2]) {
                includeHeight(neighbors[2]->heights()[(SIDE - 1U) * SIDE + offset]);
            } else {
                includeHeight(0U);
            }
            if (neighbors[3]) {
                includeHeight(neighbors[3]->heights()[offset]);
            } else {
                includeHeight(0U);
            }
        }
        elevation = m_preview_tile_elevations.insert_or_assign(key, PreviewTileElevation{
            .tile_token = tile->token(),
            .neighbor_tokens = neighbor_tokens,
            .minimum = minimum,
            .maximum = maximum,
        }).first;
    }
    shared::HeightTileSurfaceBounds const surface_bounds = shared::HeightTileSurfaceMesher::boundsForTile(
        { .x = m_height_tile_center->x, .y = m_height_tile_center->y },
        { .x = tile->key().x, .y = tile->key().y },
        elevation->second.minimum,
        elevation->second.maximum
    );
    auto const current = m_preview_mesh_details.find(key);
    std::optional<shared::HeightTileSurfaceDetail> const current_detail = current
            == m_preview_mesh_details.end()
        ? std::nullopt
        : std::optional<shared::HeightTileSurfaceDetail>{ current->second };
    shared::HeightTileSurfaceLodPolicy const policy{ m_preview_mesh_projection };
    shared::HeightTileSurfaceDetail const selected = policy.detailFor(
        m_lod_viewer_bounds,
        surface_bounds,
        current_detail
    );
    m_preview_mesh_details.insert_or_assign(key, selected);
    return selected;
}

void PlayerClient::queuePreviewMesh(HeightTileKey const key)
{
    if (!m_height_tile_interest.contains(key) || !heightTileResidency().resident(key)) {
        return;
    }
    if (m_preview_mesh_jobs.contains(key)) {
        m_preview_mesh_dirty_jobs.insert(key);
    } else if (m_pending_preview_mesh_set.insert(key).second) {
        m_pending_preview_meshes.push_back(key);
    }
}

void PlayerClient::queuePreviewRemoval(HeightTileKey const key)
{
    if (m_pending_preview_removal_set.insert(key).second) {
        m_pending_preview_removals.push_back(key);
    }
}

bool PlayerClient::hasCurrentPreviewMeshCoverage() const noexcept
{
    if (m_height_tile_interest.empty()) {
        return false;
    }
    for (HeightTileKey const key : m_height_tile_interest) {
        if (!m_visible_preview_meshes.contains(key)) {
            return false;
        }
    }
    return true;
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
    auto const upload_started = std::chrono::steady_clock::now();
    for (uint32_t completed = 0U; completed < maximum_meshes; ++completed) {
        std::optional<PreviewMeshWorkerPool::Result> result = m_preview_mesh_workers->takeResult();
        if (!result.has_value()) {
            break;
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
    if (!m_preview_mesh_workers->canAccept()) {
        return;
    }
    shared::HeightTileKey const center = *m_height_tile_center;
    int8_t const heading_x = m_applied_interest_heading_x;
    int8_t const heading_y = m_applied_interest_heading_y;
    std::ranges::sort(m_pending_preview_meshes, [center, heading_x, heading_y](
        HeightTileKey const first,
        HeightTileKey const second
    ) {
        return std::tuple{
            shared::heightTileInterestPriority(center, heading_x, heading_y, first),
            first.y,
            first.x,
        } < std::tuple{
            shared::heightTileInterestPriority(center, heading_x, heading_y, second),
            second.y,
            second.x,
        };
    });
    while (!m_pending_preview_meshes.empty()) {
        HeightTileKey const key = m_pending_preview_meshes.front();
        HeightTileHandle const tile = heightTileResidency().resident(key);
        if (!m_height_tile_interest.contains(key) || !tile) {
            m_pending_preview_mesh_set.erase(key);
            m_pending_preview_meshes.pop_front();
            continue;
        }
        if (m_preview_mesh_jobs.contains(key)) {
            m_pending_preview_mesh_set.erase(key);
            m_pending_preview_meshes.pop_front();
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
        m_pending_preview_mesh_set.erase(key);
        m_pending_preview_meshes.pop_front();
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

void PlayerClient::beginContinuousLook() noexcept
{
    glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

shared::Direction PlayerClient::input() {
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
    m_jump_queued = m_jump_queued
        || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_SPACE) == GLFW_PRESS;
    updateFlightControlToggles();

    uint32_t width = 0U;
    uint32_t height = 0U;
    m_window.framebufferSize(width, height);
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
        m_window.nativeHandle(),
        GLFW_KEY_F5,
        GLFW_KEY_5
    );
    if (m_camera_perspective_latch.update(camera_perspective_pressed)) {
        m_camera_perspective = nextCameraPerspective(m_camera_perspective);
    }
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const renderer_deadline = now + std::chrono::milliseconds{ 8 };
    shared::HeightTileSurfaceProjection const mesh_projection{
        .vertical_fov_degrees = m_camera.projection().vertical_fov_degrees,
        .viewport_width_pixels = width > 0U ? width : m_preview_mesh_projection.viewport_width_pixels,
        .viewport_height_pixels = height > 0U ? height : m_preview_mesh_projection.viewport_height_pixels,
    };
    if (auto const player = m_world.playerByCharacter(m_local_character)) {
        refreshHeightTileInterest(*player, mesh_projection);
    }
    processPendingPreviewMeshes(MAX_PREVIEW_MESHES_PER_FRAME, renderer_deadline);
    for (HeightTileChange const change : heightTileResidency().takeChanges(MAX_HEIGHT_TILE_CHANGES_PER_FRAME)) {
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
    for (uint32_t removed = 0U;
         removed < MAX_HEIGHT_TILE_CHANGES_PER_FRAME
         && hasCurrentPreviewMeshCoverage()
         && !m_pending_preview_removals.empty();
         ++removed) {
        HeightTileKey const key = m_pending_preview_removals.front();
        m_pending_preview_removals.pop_front();
        m_pending_preview_removal_set.erase(key);
        if (!m_height_tile_interest.contains(key)) {
            static_cast<void>(m_renderer.removeHeightTileMesh({.x = key.x, .y = key.y}));
            m_visible_preview_meshes.erase(key);
            m_visible_preview_mesh_tiles.erase(key);
            m_visible_preview_mesh_bases.erase(key);
            m_visible_preview_mesh_seam_bridges.erase(key);
            static constexpr std::array<size_t, 4> OPPOSITE_EDGES{ 1U, 0U, 3U, 2U };
            size_t edge_index = 0U;
            for (HeightTileKey const adjacent : {
                     offsetHeightTileKey(key, -1, 0),
                     offsetHeightTileKey(key, 1, 0),
                     offsetHeightTileKey(key, 0, -1),
                     offsetHeightTileKey(key, 0, 1),
                 }) {
                auto const seams = m_visible_preview_mesh_seam_bridges.find(adjacent);
                if (seams != m_visible_preview_mesh_seam_bridges.end()) {
                    PreviewMeshSeamBridgeSet candidate_bridges = seams->second;
                    if (candidate_bridges.replace(OPPOSITE_EDGES[edge_index], {})) {
                        static_cast<void>(refreshVisiblePreviewMesh(
                            adjacent,
                            renderer_deadline,
                            &candidate_bridges
                        ));
                    }
                }
                ++edge_index;
            }
        }
    }
    processPendingPreviewMeshes(MAX_PREVIEW_MESHES_PER_FRAME, renderer_deadline);
    std::optional<PlayerPresentationPosition> const local_position = predictedLocalPresentation(now);
    double camera_distance = MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
    if (local_position.has_value()) {
        camera_distance = maximumUnobstructedCameraDistance(*local_position);
        PlayerCameraView const camera_view = resolveLocalPlayerCamera(
            *local_position,
            m_look_camera.pose().angles,
            m_camera_perspective,
            camera_distance
        );
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
        RendererRuntimeInfo const runtime = m_renderer.runtimeInfo();
        if (!m_capture_pre_rotation_interest.has_value()
            && runtime.height_tile_mesh_count >= m_capture->minimum_height_tile_meshes) {
            m_capture_pre_rotation_interest = m_height_tile_interest;
            CameraAngles angles = m_look_camera.pose().angles;
            // The flight spawn is east of the world center. Rotate west so the
            // acceptance capture also proves that the central peak remains drawn.
            angles.yaw_degrees -= 90.0;
            static_cast<void>(m_look_camera.setAngles(angles));
            m_capture_rotation_frames = 1U;
        } else if (m_capture_pre_rotation_interest.has_value() && !m_capture_requested) {
            ++m_capture_rotation_frames;
            if (m_capture_rotation_frames >= 60U) {
                if (m_height_tile_interest != *m_capture_pre_rotation_interest) {
                    CORE_ERROR("Camera rotation changed the resident terrain set");
                    m_running = false;
                    return;
                }
                m_renderer.requestFrameCapture();
                m_capture_requested = true;
            }
        }
        if (std::optional<RendererFrameCapture> capture = m_renderer.takeFrameCapture(); capture.has_value()) {
            std::ofstream output{m_capture->image_path, std::ios::binary | std::ios::trunc};
            output << "P6\n" << capture->width << ' ' << capture->height << "\n255\n";
            for (size_t pixel = 0U; pixel < capture->rgba8.size(); pixel += 4U) {
                output.write(reinterpret_cast<char const*>(capture->rgba8.data() + pixel), 3);
            }
            if (!output) {
                CORE_ERROR("Failed to write playtest capture {}", m_capture->image_path.string());
            }
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

    glm::dvec3 const eye = localPlayerEyePosition(local_position);
    PlayerCameraView const intended_camera = resolveLocalPlayerCamera(
        local_position,
        m_look_camera.pose().angles,
        m_camera_perspective,
        MAX_LOCAL_PLAYER_CAMERA_DISTANCE
    );
    glm::dvec3 const ray = (intended_camera.pose.position - eye)
        / MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
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
        MAX_LOCAL_PLAYER_CAMERA_DISTANCE,
        CAMERA_OBSTRUCTION_SAMPLE_COUNT,
        CAMERA_OBSTRUCTION_BINARY_STEPS,
        CAMERA_OBSTRUCTION_MARGIN,
        obstructed
    );
}

} // namespace client
