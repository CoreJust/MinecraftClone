#include <client/PlayerClient.hpp>

#include <client/CameraController.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewMeshing.hpp>

#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>

#include <core/IO/Log.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
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
    };

    struct Result final {
        HeightTileKey key;
        HeightTileHandle tile;
        shared::HeightTileSurfaceMesh mesh;
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
            Result result{
                .key = job.key,
                .tile = job.source.tile,
                .mesh = buildPreviewMesh(job.source),
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

PlayerClient::PlayerClient(
    shared::WorldMode const mode,
    std::optional<PlayerClientCaptureOptions> capture
)
    : GameClient{ mode }
    , m_capture(std::move(capture))
    , m_window(core::platform::glfw::WindowDescriptor{
        .width = 1280U,
        .height = 720U,
        .title = std::string{ shared::PROJECT_NAME },
    })
    , m_renderer(
        VulkanRenderer::createPresentationContext(
            m_window,
            {.enable_frame_capture = m_capture.has_value()}
        ),
        m_shader_assets,
        {.enable_frame_capture = m_capture.has_value()}
    )
    , m_preview_mesh_workers(std::make_unique<PreviewMeshWorkerPool>())
{
    beginContinuousLook();
    static_cast<void>(m_camera.setProjection({ .far_plane = 1'200.0 }));
    m_renderer.setDebugHudEnabled(true);
}

size_t PlayerHeightTileKeyHash::operator()(shared::HeightTileKey const key) const noexcept
{
    return static_cast<size_t>((static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32U)
        ^ static_cast<uint32_t>(key.y));
}

void PlayerClient::refreshHeightTileInterest(shared::Player const& player)
{
    shared::HeightTileKey const center = shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(player.x),
        .y = floorDivideByHeightTileSide(player.y),
    });
    shared::HeightTileHeading const heading = shared::canonicalHeightTileHeading(
        m_interest_heading_x, m_interest_heading_y
    );
    if (m_height_tile_center == center
        && m_applied_interest_heading_x == heading.x
        && m_applied_interest_heading_y == heading.y) {
        return;
    }
    shared::HeightTileInterest const next = shared::makeHeightTileInterest(
        center, heading.x, heading.y
    );
    std::unordered_set<HeightTileKey, PlayerHeightTileKeyHash> next_interest{
        next.keys.begin(), next.keys.end()
    };
    if (m_height_tile_center == center && m_height_tile_interest == next_interest) {
        m_applied_interest_heading_x = heading.x;
        m_applied_interest_heading_y = heading.y;
        m_preview_mesh_workers->setPriority(center, heading.x, heading.y);
        return;
    }
    auto const add = [this](HeightTileKey const key) {
        if (m_height_tile_interest.insert(key).second) {
            queuePreviewMesh(key);
        }
    };
    auto const remove = [this](HeightTileKey const key) {
        if (m_height_tile_interest.erase(key) > 0U) {
            m_pending_preview_removals.push_back(key);
        }
    };
    std::vector<HeightTileKey> departed;
    departed.reserve(m_height_tile_interest.size());
    for (HeightTileKey const key : m_height_tile_interest) {
        if (!next_interest.contains(key)) {
            departed.push_back(key);
        }
    }
    for (HeightTileKey const key : departed) {
        remove(key);
    }
    for (HeightTileKey const key : next.keys) {
        if (!m_height_tile_interest.contains(key)) {
            add(key);
        }
    }
    m_height_tile_center = center;
    m_applied_interest_heading_x = heading.x;
    m_applied_interest_heading_y = heading.y;
    std::erase_if(m_pending_preview_meshes, [this](HeightTileKey const key) {
        if (m_height_tile_interest.contains(key)) {
            return false;
        }
        m_pending_preview_mesh_set.erase(key);
        return true;
    });
    m_preview_mesh_workers->setPriority(center, heading.x, heading.y);
    for (HeightTileKey const key : m_preview_mesh_workers->cancelQueuedOutsideInterest(m_height_tile_interest)) {
        m_preview_mesh_jobs.erase(key);
        m_preview_mesh_dirty_jobs.erase(key);
    }
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

void PlayerClient::processPendingPreviewMeshes(uint32_t const maximum_meshes)
{
    if (!m_height_tile_center.has_value()) {
        return;
    }
    auto const upload_started = std::chrono::steady_clock::now();
    for (uint32_t completed = 0U; completed < maximum_meshes; ++completed) {
        std::optional<PreviewMeshWorkerPool::Result> result = m_preview_mesh_workers->takeResult();
        if (!result.has_value()) {
            break;
        }
        m_preview_mesh_jobs.erase(result->key);
        HeightTileHandle const current = heightTileResidency().resident(result->key);
        if (m_height_tile_interest.contains(result->key) && current == result->tile) {
            m_renderer.upsertHeightTileMesh(result->mesh);
        }
        if (m_preview_mesh_dirty_jobs.erase(result->key) > 0U || current != result->tile) {
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
        PreviewMeshWorkerPool::Job job{
            .key = key,
            .source = {
                .tile = tile,
                .neighbors = {
                    heightTileResidency().resident(offsetHeightTileKey(key, -1, 0)),
                    heightTileResidency().resident(offsetHeightTileKey(key, 1, 0)),
                    heightTileResidency().resident(offsetHeightTileKey(key, 0, -1)),
                    heightTileResidency().resident(offsetHeightTileKey(key, 0, 1)),
                },
            },
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
        m_camera.pose().angles.yaw_degrees
    );
    glm::dvec3 const view = m_camera.forward();
    int8_t const view_x = CameraController::quantize(view.x);
    int8_t const view_y = CameraController::quantize(view.y);
    m_interest_heading_x = movement.x != 0 || movement.y != 0 ? movement.x : view_x;
    m_interest_heading_y = movement.x != 0 || movement.y != 0 ? movement.y : view_y;
    return shared::Direction{
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
        .z = static_cast<uint8_t>(CameraController::verticalMovement(
            glfwGetKey(m_window.nativeHandle(), GLFW_KEY_SPACE) == GLFW_PRESS,
            glfwGetKey(m_window.nativeHandle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
            || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS
        )),
        .accelerated = m_acceleration_enabled,
        .speedup = shared::FLIGHT_SPEEDUP_PROFILES[m_speedup_profile_index],
        .view_x = view_x,
        .view_y = view_y,
    };
}

void PlayerClient::render() {
    if (!m_window.nextFrame()) {
        m_running = false;
        return;
    }
    updateFlightControlToggles();

    uint32_t width = 0U;
    uint32_t height = 0U;
    m_window.framebufferSize(width, height);
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(m_window.nativeHandle(), &cursor_x, &cursor_y);
    if (m_has_cursor_position) {
        static_cast<void>(m_camera.rotate(
            (cursor_x - m_last_cursor_x) * 0.15,
            (m_last_cursor_y - cursor_y) * 0.15
        ));
    }
    m_last_cursor_x = cursor_x;
    m_last_cursor_y = cursor_y;
    m_has_cursor_position = true;
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    if (auto const player = m_world.playerByCharacter(m_local_character)) {
        refreshHeightTileInterest(*player);
    }
    processPendingPreviewMeshes(MAX_PREVIEW_MESHES_PER_FRAME);
    for (HeightTileChange const change : heightTileResidency().takeChanges(MAX_HEIGHT_TILE_CHANGES_PER_FRAME)) {
        if (change.kind == HeightTileChangeKind::Remove) {
            static_cast<void>(m_renderer.removeHeightTileMesh({.x = change.key.x, .y = change.key.y}));
        } else {
            queuePreviewMesh(change.key);
        }
        queuePreviewMesh(offsetHeightTileKey(change.key, -1, 0));
        queuePreviewMesh(offsetHeightTileKey(change.key, 1, 0));
        queuePreviewMesh(offsetHeightTileKey(change.key, 0, -1));
        queuePreviewMesh(offsetHeightTileKey(change.key, 0, 1));
    }
    for (uint32_t removed = 0U;
         removed < MAX_HEIGHT_TILE_CHANGES_PER_FRAME && !m_pending_preview_removals.empty();
         ++removed) {
        HeightTileKey const key = m_pending_preview_removals.front();
        m_pending_preview_removals.pop_front();
        if (!m_height_tile_interest.contains(key)) {
            static_cast<void>(m_renderer.removeHeightTileMesh({.x = key.x, .y = key.y}));
        }
    }
    std::optional<PlayerPresentationPosition> const local_position = predictedLocalPresentation(now);
    if (local_position.has_value()) {
        CameraPose const camera_pose = localPlayerFirstPersonPose(*local_position, m_camera.pose().angles);
        static_cast<void>(m_camera.setPosition(camera_pose.position));
    }
    m_renderer.recreate(width, height);
    m_render_data.clear();
    m_render_data.reserve(m_world.players().size());
    for (shared::Player const& p : m_world.players()) {
        if (p.ch == m_local_character) {
            continue;
        }
        if (auto const position = m_player_presentation.sample(p.ch, now)) {
            m_render_data.push_back({
                .x = static_cast<float>(position->x),
                .y = static_cast<float>(position->y),
                .color = { float(p.ch) / 256.f, 1.f - float(p.ch) / 256.f, 1.f, 1.f },
                .z = static_cast<float>(position->z),
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
        return input;
    }();
    float content_scale_x = 1.0F;
    float content_scale_y = 1.0F;
    glfwGetWindowContentScale(m_window.nativeHandle(), &content_scale_x, &content_scale_y);
    bool const debug_hud_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_F1) == GLFW_PRESS;
    if (m_debug_hud_toggle.update(debug_hud_pressed)) {
        m_renderer.toggleDebugHud();
    }
    m_renderer.setCamera(m_camera.pose());
    static_cast<void>(m_renderer.render(
        m_render_data,
        debug_hud_input,
        std::max(content_scale_x, content_scale_y),
        std::chrono::steady_clock::now() + std::chrono::milliseconds{8}
    ));

    if (m_capture.has_value()) {
        RendererRuntimeInfo const runtime = m_renderer.runtimeInfo();
        if (!m_capture_pre_rotation_interest.has_value()
            && runtime.height_tile_mesh_count >= m_capture->minimum_height_tile_meshes) {
            m_capture_pre_rotation_interest = m_height_tile_interest;
            CameraAngles angles = m_camera.pose().angles;
            // The flight spawn is east of the world center. Rotate west so the
            // acceptance capture also proves that the central peak remains drawn.
            angles.yaw_degrees -= 90.0;
            static_cast<void>(m_camera.setAngles(angles));
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

} // namespace client
