#include <acceptance/GameBenchmark.hpp>

#include <client/Camera.hpp>
#include <client/GameClient.hpp>
#include <client/PlayerClient.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/WorldLoading.hpp>
#include <client/render/PresentationFrameProgress.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>
#include <shared/ProjectInfo.hpp>
#include <shared/corelang/Backend.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>
#include <core/graphics/vulkan/Vulkan.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/RendererBenchmark.hpp>
#include <acceptance/ReplayInput.hpp>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace acceptance {

namespace {

[[nodiscard]]
bool environmentVariableMatches(
    char const* const name,
    std::optional<std::string_view> const expected_value
) noexcept
{
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t value_size = 0U;
    auto const status = ::_dupenv_s(&value, &value_size, name);
    bool const matches = status != 0 || (value != nullptr
        && (!expected_value || std::string_view{ value } == *expected_value));
    std::free(value);
    return matches;
#else
    char const* const value = std::getenv(name);
    return value != nullptr
        && (!expected_value || std::string_view{ value } == *expected_value);
#endif
}

using Clock = std::chrono::steady_clock;

struct PresentationRequestKey final {
    uint64_t swapchain_generation = 0U;
    uint32_t application_present_id = 0U;

    bool operator==(PresentationRequestKey const&) const noexcept = default;
};

// Reuse large per-loop values without reserving their storage on the callback stack.
template <typename T>
class ReusableHeapValue final {
public:
    ReusableHeapValue() = default;
    ReusableHeapValue(ReusableHeapValue const&) = delete;
    ReusableHeapValue& operator=(ReusableHeapValue const&) = delete;
    ReusableHeapValue(ReusableHeapValue&&) = delete;
    ReusableHeapValue& operator=(ReusableHeapValue&&) = delete;

    ~ReusableHeapValue() { reset(); }

    template <typename Factory>
    T& replaceFrom(Factory&& factory)
    {
        reset();
        T* const value = ::new (static_cast<void*>(m_storage)) T(std::forward<Factory>(factory)());
        m_value = value;
        return *value;
    }

    template <typename Factory>
    T& replaceWith(Factory&& factory)
    {
        reset();
        m_value = std::forward<Factory>(factory)(static_cast<void*>(m_storage));
        return *m_value;
    }

private:
    void reset() noexcept
    {
        if (m_value != nullptr) {
            std::destroy_at(m_value);
            m_value = nullptr;
        }
    }

    alignas(T) std::byte m_storage[sizeof(T)];
    T* m_value = nullptr;
};

struct PresentationRequestKeyHash final {
    size_t operator()(PresentationRequestKey const& key) const noexcept
    {
        size_t const generation_hash = std::hash<uint64_t>{}(key.swapchain_generation);
        size_t const present_id_hash = std::hash<uint32_t>{}(key.application_present_id);
        return generation_hash ^ (present_id_hash + static_cast<size_t>(0x9e37'79b9U)
            + (generation_hash << 6U) + (generation_hash >> 2U));
    }
};

[[nodiscard]]
PresentationRequestKey presentationRequestKey(GameBenchmarkPresentationRequestId const& request) noexcept
{
    return {
        .swapchain_generation = request.swapchain_generation,
        .application_present_id = request.application_present_id,
    };
}

[[nodiscard]]
uint64_t positiveNanoseconds(std::chrono::nanoseconds const duration) noexcept
{
    return duration.count() > 0 ? static_cast<uint64_t>(duration.count()) : 0U;
}

std::optional<uint64_t> benchmarkOffsetFromStart(
    std::optional<Clock::time_point> const benchmark_started_at,
    Clock::time_point const sampled_at
) noexcept
{
    if (!benchmark_started_at || sampled_at < *benchmark_started_at) {
        return std::nullopt;
    }
    return positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
        sampled_at - *benchmark_started_at
    ));
}

std::optional<int64_t> signedBenchmarkOffsetFromStart(
    std::optional<Clock::time_point> const benchmark_started_at,
    Clock::time_point const sampled_at
) noexcept
{
    if (!benchmark_started_at) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        sampled_at - *benchmark_started_at
    ).count();
}

[[nodiscard]]
GameBenchmarkTerrainObservation gameBenchmarkTerrainObservation(
    client::PlayerClientBenchmarkTerrainInfo const& info
)
{
    GameBenchmarkTerrainObservation observation{
        .center = info.center,
        .desired_count = info.desired_count,
        .resident_count = info.resident_count,
        .nonresident_count = info.nonresident_count,
        .current_mesh_visible_count = info.current_mesh_visible_count,
        .pending_count = info.pending_count,
        .active_count = info.active_count,
        .unclassified_missing_count = info.unclassified_missing_count,
        .accepted_near_admissions = info.accepted_near_admissions,
        .accepted_far_admissions = info.accepted_far_admissions,
        .far_admissions_while_near_pending = info.far_admissions_while_near_pending,
    };
    static_assert(
        client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
        == GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
    );
    for (uint32_t index = 0U; index < client::PLAYER_CLIENT_BENCHMARK_TERRAIN_NEAR_TILE_COUNT; ++index) {
        client::PlayerClientBenchmarkTerrainTileState const& source = info.near_tiles[index];
        observation.near_tiles[index] = GameBenchmarkTerrainTileObservation{
            .key = source.key,
            .desired = source.desired,
            .resident = source.resident,
            .current_mesh_visible = source.current_mesh_visible,
            .pending = source.pending,
            .active = source.active,
        };
    }
    return observation;
}

[[nodiscard]]
GameBenchmarkPresentationAcquireStage gameBenchmarkPresentationAcquireStage(
    core::graphics::vulkan::PresentationAcquireStage const stage
)
{
    switch (stage) {
    case core::graphics::vulkan::PresentationAcquireStage::SubmittedFrameFence:
        return GameBenchmarkPresentationAcquireStage::SubmittedFrameFence;
    case core::graphics::vulkan::PresentationAcquireStage::AcquireFence:
        return GameBenchmarkPresentationAcquireStage::AcquireFence;
    case core::graphics::vulkan::PresentationAcquireStage::PresentFence:
        return GameBenchmarkPresentationAcquireStage::PresentFence;
    case core::graphics::vulkan::PresentationAcquireStage::SwapchainImage:
        return GameBenchmarkPresentationAcquireStage::SwapchainImage;
    }
    throw std::runtime_error("unknown Vulkan presentation acquisition stage");
}

[[nodiscard]]
GameBenchmarkPresentationAcquireSample gameBenchmarkPresentationAcquireSample(
    core::graphics::vulkan::PresentationAcquireDiagnostics const& diagnostics
)
{
    if (diagnostics.stage_count > diagnostics.stages.size()) {
        throw std::runtime_error("Vulkan presentation acquisition diagnostics exceed their fixed bound");
    }
    GameBenchmarkPresentationAcquireSample sample{
        .captured = diagnostics.captured,
        .frame_slot = diagnostics.frame_slot,
        .stage_count = diagnostics.stage_count,
        .image_index = diagnostics.image_index,
    };
    for (uint32_t index = 0U; index < diagnostics.stage_count; ++index) {
        core::graphics::vulkan::PresentationAcquireStageTiming const& source = diagnostics.stages[index];
        sample.stages[index] = {
            .stage = gameBenchmarkPresentationAcquireStage(source.stage),
            .duration_ns = positiveNanoseconds(source.duration),
            .result = static_cast<int32_t>(source.result),
        };
    }
    return sample;
}

[[nodiscard]]
GameBenchmarkFrameSample::TerrainNearSample gameBenchmarkTerrainFrameSample(
    client::PlayerClientBenchmarkTerrainInfo const& info,
    uint64_t const accepted_near_admission_delta,
    uint64_t const accepted_far_admission_delta,
    uint64_t const far_admission_while_near_pending_delta
)
{
    GameBenchmarkFrameSample::TerrainNearSample sample{
        .center_valid = info.center.has_value(),
        .desired_tile_count = info.desired_count,
        .resident_tile_count = info.resident_count,
        .nonresident_tile_count = info.nonresident_count,
        .current_mesh_visible_count = info.current_mesh_visible_count,
        .pending_mesh_count = info.pending_count,
        .active_mesh_job_count = info.active_count,
        .unclassified_missing_count = info.unclassified_missing_count,
        .accepted_near_admission_delta = accepted_near_admission_delta,
        .accepted_far_admission_delta = accepted_far_admission_delta,
        .far_admission_while_near_pending_delta = far_admission_while_near_pending_delta,
        .mesh_work_stages = GameBenchmarkMeshStageSummary{
            .client_pending_admission_count = info.client_pending_admission_count,
            .worker_queued_count = info.worker_queued_count,
            .worker_building_count = info.worker_building_count,
            .completed_result_waiting_count = info.completed_result_waiting_count,
            .oldest_client_pending_near_observed_age_ns = positiveNanoseconds(
                info.oldest_client_pending_near_observed_age
            ),
            .oldest_worker_queued_age_ns = positiveNanoseconds(info.oldest_worker_queued_age),
            .oldest_worker_building_age_ns = positiveNanoseconds(info.oldest_worker_building_age),
            .oldest_completed_result_waiting_age_ns = positiveNanoseconds(
                info.oldest_completed_result_waiting_age
            ),
        },
        .mesh_result_drain = GameBenchmarkMeshResultDrainSummary{
            .drained_result_count = info.mesh_result_drain.drained_result_count,
            .stale_epoch_discard_count = info.mesh_result_drain.stale_epoch_discard_count,
            .no_longer_interested_or_resident_discard_count =
                info.mesh_result_drain.no_longer_interested_or_resident_discard_count,
            .source_mismatch_requeue_count = info.mesh_result_drain.source_mismatch_requeue_count,
            .target_detail_mismatch_requeue_count = info.mesh_result_drain.target_detail_mismatch_requeue_count,
            .cannot_publish_requeue_count = info.mesh_result_drain.cannot_publish_requeue_count,
            .publish_attempt_count = info.mesh_result_drain.publish_attempt_count,
            .publish_success_count = info.mesh_result_drain.publish_success_count,
            .publish_failure_count = info.mesh_result_drain.publish_failure_count,
            .seam_planning_duration_ns = positiveNanoseconds(info.mesh_result_drain.seam_planning_duration),
            .renderer_batch_upsert_duration_ns = positiveNanoseconds(
                info.mesh_result_drain.renderer_batch_upsert_duration
            ),
            .renderer_free_range_prepare_duration_ns = positiveNanoseconds(
                info.mesh_result_drain.renderer_upsert_stages.free_range_prepare_duration
            ),
            .renderer_mesh_prepare_duration_ns = positiveNanoseconds(
                info.mesh_result_drain.renderer_upsert_stages.mesh_prepare_duration
            ),
            .renderer_capacity_prepare_duration_ns = positiveNanoseconds(
                info.mesh_result_drain.renderer_upsert_stages.capacity_prepare_duration
            ),
            .renderer_commit_upload_duration_ns = positiveNanoseconds(
                info.mesh_result_drain.renderer_upsert_stages.commit_upload_duration
            ),
        },
    };
    if (info.center) {
        sample.center_x = info.center->x;
        sample.center_y = info.center->y;
    }
    return sample;
}

[[nodiscard]]
std::string hardwareName()
{
    std::string cpu_name{ "unknown" };
    uint64_t ram_bytes = 0U;
#if defined(__APPLE__)
    size_t name_size = 0U;
    if (sysctlbyname("machdep.cpu.brand_string", nullptr, &name_size, nullptr, 0) == 0
        && name_size > 1U) {
        std::string name(name_size, '\0');
        if (sysctlbyname("machdep.cpu.brand_string", name.data(), &name_size, nullptr, 0) == 0) {
            name.pop_back();
            cpu_name = std::move(name);
        }
    }
    size_t memory_size = sizeof(ram_bytes);
    static_cast<void>(sysctlbyname("hw.memsize", &ram_bytes, &memory_size, nullptr, 0));
#elif defined(_WIN32)
    std::array<char, 256> processor_identifier{};
    DWORD const length = GetEnvironmentVariableA(
        "PROCESSOR_IDENTIFIER",
        processor_identifier.data(),
        static_cast<DWORD>(processor_identifier.size())
    );
    if (length > 0U && length < processor_identifier.size()) {
        cpu_name = processor_identifier.data();
    }
    MEMORYSTATUSEX memory_status{ .dwLength = sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memory_status) != 0) {
        ram_bytes = memory_status.ullTotalPhys;
    }
#endif
    return "cpu=" + cpu_name + "; logical_cpus="
        + std::to_string(std::thread::hardware_concurrency()) + "; ram_bytes="
        + std::to_string(ram_bytes) + "; power_mode=unavailable";
}

[[nodiscard]]
std::string presentModeName(client::RendererPresentMode const mode)
{
    switch (mode) {
        case client::RendererPresentMode::Immediate: return "immediate";
        case client::RendererPresentMode::Mailbox: return "mailbox";
        case client::RendererPresentMode::FIFO: return "fifo";
        case client::RendererPresentMode::FIFORelaxed: return "fifo-relaxed";
        case client::RendererPresentMode::Unknown: return "unknown";
    }
    return "unknown";
}

class ServerRunGuard final {
public:
    ServerRunGuard(server::GameServer& server, server::GameServer::BenchmarkHooks const& hooks)
        : m_thread([this, &server, &hooks] { server.run(m_stop, &hooks); })
    { }

    ~ServerRunGuard()
    {
        stopAndJoin();
    }

    void stopAndJoin()
    {
        m_stop.store(true, std::memory_order_relaxed);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    std::atomic_bool m_stop{ false };
    std::thread m_thread;
};

struct ServerTickSample final {
    Clock::time_point completed_at;
    std::chrono::nanoseconds duration;
    uint64_t network_events;
};

shared::PolicyCapabilityRegistry movementPolicyRegistry()
{
    return {
        .definitions = {
            {
                .key = "minecraft:flight",
                .default_value = 1,
                .minimum_value = 0,
                .maximum_value = 1,
                .hard_restriction = shared::PolicyRestriction::Maximum,
            },
            {
                .key = "minecraft:collision-bypass",
                .default_value = 1,
                .minimum_value = 0,
                .maximum_value = 1,
                .hard_restriction = shared::PolicyRestriction::Maximum,
            },
        },
    };
}

std::expected<shared::PolicyCompilation, shared::PolicyDiagnostic> compileMovementPolicy(
    bool const collision_bypass_allowed
)
{
    static constexpr std::string_view ALLOW_SOURCE = R"core(@version(0.1.5)
@use minecraft
pub fn policy() {
    policyRule("collision", "minecraft:collision-bypass", 1i64, 1u8)
    policyAssign("all", "collision", 1u8)
}
)core";
    static constexpr std::string_view DENY_SOURCE = R"core(@version(0.1.5)
@use minecraft
pub fn policy() {
    policyRule("collision", "minecraft:collision-bypass", 0i64, 1u8)
    policyAssign("all", "collision", 1u8)
}
)core";
    shared::PolicyHost compiler;
    return compiler.compile(
        "benchmark-movement.core",
        collision_bypass_allowed ? ALLOW_SOURCE : DENY_SOURCE,
        {
            .backend = shared::PolicyExecutionBackend::Interpreter,
            .require_interpreter_parity = true,
        }
    );
}

std::string_view fullRadiusPhaseName(GameBenchmarkFullRadiusPhase const phase) noexcept
{
    switch (phase) {
        case GameBenchmarkFullRadiusPhase::InitialWarmup: return "initial-full-radius-warmup";
        case GameBenchmarkFullRadiusPhase::BaselineStationary: return "baseline-stationary";
        case GameBenchmarkFullRadiusPhase::Movement: return "full-radius-movement";
        case GameBenchmarkFullRadiusPhase::RecoveryWarmup: return "full-radius-recovery-warmup";
        case GameBenchmarkFullRadiusPhase::RecoveredStationary: return "recovered-stationary";
        case GameBenchmarkFullRadiusPhase::Complete: return "complete";
        case GameBenchmarkFullRadiusPhase::Failed: return "failed";
    }
    return "unknown";
}

std::string_view worldLoadingStateName(client::WorldLoadingState const state) noexcept
{
    switch (state) {
        case client::WorldLoadingState::Idle: return "Idle";
        case client::WorldLoadingState::Creating: return "Creating";
        case client::WorldLoadingState::PreparingAtlas: return "PreparingAtlas";
        case client::WorldLoadingState::PreparingSpawn: return "PreparingSpawn";
        case client::WorldLoadingState::ReadyForStart: return "ReadyForStart";
        case client::WorldLoadingState::Starting: return "Starting";
        case client::WorldLoadingState::Playing: return "Playing";
        case client::WorldLoadingState::Failed: return "Failed";
        case client::WorldLoadingState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

bool playableSphereReady(client::WorldLoadingStatus const& status) noexcept
{
    return status.state == client::WorldLoadingState::Playing
        && status.playable_sphere_ready
        && status.playable_sphere_ready_chunks > 0U
        && status.playable_sphere_ready_chunks == status.playable_sphere_required_chunks;
}

shared::PlayerPosition playerPosition(shared::Player const& player) noexcept
{
    return shared::PlayerPosition{
        .x = player.x,
        .y = player.y,
        .z = player.z,
        .x_subcell = player.x_subcell,
        .y_subcell = player.y_subcell,
        .z_subcell = player.z_subcell,
    };
}

bool samePlayerPosition(shared::PlayerPosition const left, shared::PlayerPosition const right) noexcept
{
    return left.x == right.x && left.y == right.y && left.z == right.z
        && left.x_subcell == right.x_subcell
        && left.y_subcell == right.y_subcell
        && left.z_subcell == right.z_subcell;
}

GameBenchmarkFrameSample::PlayableSphere playableSphereSample(
    client::WorldLoadingStatus const& status,
    std::optional<uint64_t> const active_coverage_epoch,
    std::optional<shared::PlayerPosition> const active_coverage_center,
    uint32_t const radius_blocks = shared::MAX_SPHERE_RADIUS_BLOCKS,
    shared::SparseWorld const* const authoritative_world = nullptr
)
{
    bool const full_volume = radius_blocks == shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    GameBenchmarkFrameSample::PlayableSphere sample{
        .radius_blocks = radius_blocks,
        .active_coverage_epoch = active_coverage_epoch,
        .active_coverage_center = active_coverage_center,
        .world_loading_center = status.current_player_position,
        .ready_chunks = full_volume ? status.playable_sphere_ready_chunks : 0U,
        .required_chunks = full_volume ? status.playable_sphere_required_chunks : 0U,
        .ready = full_volume && playableSphereReady(status),
        .coverage_scope = full_volume ? "horizontal_full_volume" : "",
        .required_authoritative_columns = full_volume ? status.required_authoritative_columns : 0U,
        .resident_authoritative_columns = full_volume ? status.resident_authoritative_columns : 0U,
        .vertical_chunk_count = full_volume
            ? (shared::World::FLIGHT_MAX_Z + 1U) / shared::Chunk::SIDE_LENGTH : 0U,
        .authoritative_columns_center = full_volume ? status.current_player_position : std::nullopt,
    };
    if (!full_volume && authoritative_world != nullptr) {
        gameBenchmarkObserveGeometricSphere(sample, *authoritative_world);
    }
    return sample;
}

bool playableSphereSampleReady(
    GameBenchmarkFrameSample::PlayableSphere const& sample,
    uint32_t const expected_radius_blocks = shared::MAX_SPHERE_RADIUS_BLOCKS
) noexcept
{
    return sample.radius_blocks == expected_radius_blocks
        && sample.active_coverage_epoch.has_value()
        && sample.active_coverage_center.has_value()
        && sample.world_loading_center.has_value()
        && sample.required_chunks > 0U
        && sample.ready_chunks == sample.required_chunks
        && sample.ready;
}

bool s1PlayableSphereReady(
    client::PlayerClient const& player,
    client::WorldLoadingStatus const& status
)
{
    return playableSphereReady(status)
        && gameBenchmarkS1CurrentTerrainSampleReady(playableSphereSample(
            status,
            player.activeCoverageEpoch(),
            player.activeCoverageCenter(),
            status.actual_chunk_radius_blocks
        ));
}

GameBenchmarkFrameSample::VisualLod gameBenchmarkVisualLodSample(
    client::PlayerClient const& player,
    client::WorldLoadingStatus const& status,
    client::RendererRuntimeInfo const& renderer
)
{
    GameBenchmarkFrameSample::VisualLod sample{
        .radius_blocks = status.visual_lod_radius_blocks,
        .terrain_identity = std::nullopt,
        .submitted_frame_count = renderer.submitted_frame_count,
        .actual_radius_blocks = status.actual_chunk_radius_blocks,
        .required_actual_columns = 6'561U,
        .coverage_center = status.current_player_position,
    };
    auto const& scene = renderer.terrain_lod_scene;
    auto const session = player.activeTerrainSession();
    auto const* const world = player.authoritativeWorld();
    if (!scene || !session || world == nullptr || !sample.coverage_center) {
        return sample;
    }
    sample.scene_active = true;
    sample.leaf_count = static_cast<uint32_t>(scene->leaves.size());
    sample.actual_column_count = scene->actual_column_count;
    sample.terrain_identity = scene->identity;
    sample.session_join_epoch = scene->session_join_epoch;
    sample.coverage_epoch = scene->coverage_epoch;
    sample.scene_center_x = scene->center_x;
    sample.scene_center_y = scene->center_y;
    // The submitted scene is immutable. Reuse its proof until either the current
    // authoritative profiles or the required column window changes; retain no scene.
    struct CachedProof final {
        std::weak_ptr<client::TerrainLodScene const> scene;
        shared::SparseWorld const* world = nullptr;
        uint64_t revision = 0U;
        int32_t center_x = -1;
        int32_t center_y = -1;
        uint32_t included = 0U;
        bool sources_current = false;
    };
    thread_local CachedProof cached;
    bool const same_scene = cached.scene.lock() == scene && cached.world == world;
    int32_t const center_x = sample.coverage_center->x / shared::Chunk::SIDE_LENGTH;
    int32_t const center_y = sample.coverage_center->y / shared::Chunk::SIDE_LENGTH;
    if (!same_scene || cached.center_x != center_x || cached.center_y != center_y) {
        cached.included = gameBenchmarkS1PresentedCurrentColumnCount(*scene, *sample.coverage_center);
        cached.center_x = center_x;
        cached.center_y = center_y;
    }
    if (!same_scene || cached.revision != world->authoritativeSurfaceRevision()) {
        cached.sources_current = !scene->authority_sources.empty() && client::terrainLodSourcesCurrent(*scene, *world);
        cached.revision = world->authoritativeSurfaceRevision();
    }
    cached.scene = scene;
    cached.world = world;
    sample.presented_current_actual_columns = cached.included;
    sample.sources_current = cached.sources_current && scene->identity == session->terrain
        && scene->session_join_epoch == session->join_epoch;
    return sample;
}

bool isX30MovementInput(shared::Direction const& direction) noexcept
{
    return direction.accelerated && direction.speedup == 30U
        && (direction.x != 0U || direction.y != 0U || direction.z != 0U);
}

bool fullRadiusMovementObservationActive(
    bool const full_radius_workload,
    std::optional<Clock::time_point> const movement_started_at,
    Clock::time_point const observed_at
) noexcept
{
    return full_radius_workload
        && movement_started_at
        && observed_at >= *movement_started_at
        && observed_at < *movement_started_at + GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
}

std::optional<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
fullRadiusMovementObservation(
    client::PlayerClient const& player,
    shared::Player const& authoritative_player,
    shared::ServerPlayerPositionMessage const& server_position,
    uint64_t const update_ordinal,
    Clock::time_point const observed_at,
    std::optional<Clock::time_point> const benchmark_started_at,
    uint32_t const sample_radius_blocks = shared::MAX_SPHERE_RADIUS_BLOCKS
)
{
    client::WorldLoadingStatus const status = player.worldLoadingStatus();
    std::optional<uint64_t> const offset = benchmarkOffsetFromStart(benchmark_started_at, observed_at);
    if (!offset) {
        return std::nullopt;
    }
    return GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate{
        .update_ordinal = update_ordinal,
        .offset_ns = *offset,
        .state_revision = server_position.state_revision,
        .acknowledged_input_sequence = server_position.acknowledged_input_sequence,
        .authoritative_position = playerPosition(authoritative_player),
        .movement_capabilities = server_position.movement_capabilities.bits,
        .vertical_velocity_subcells = server_position.vertical_velocity_subcells,
        .playable_sphere = playableSphereSample(
            status,
            player.activeCoverageEpoch(),
            player.activeCoverageCenter(),
            sample_radius_blocks,
            player.authoritativeWorld()
        ),
    };
}

GameBenchmarkFrameBudgetWindow fullRadiusFrameBudgetWindow(
    std::span<GameBenchmarkFrameSample const> frames,
    std::span<GameBenchmarkFrameCompletion const> completions,
    std::optional<Clock::time_point> const first_loop_at,
    std::optional<Clock::time_point> const movement_started_at,
    bool const movement_window_complete
)
{
    GameBenchmarkFrameBudgetWindow window{};
    window.phase = fullRadiusPhaseName(GameBenchmarkFullRadiusPhase::Movement);
    window.required_duration_ns = positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
        GAME_BENCHMARK_FULL_RADIUS_MOVEMENT
    ));
    if (!first_loop_at || !movement_started_at) {
        return window;
    }

    window.start_offset_ns = positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
        *movement_started_at - *first_loop_at
    ));
    window.end_offset_ns = window.start_offset_ns + window.required_duration_ns;
    window.complete = movement_window_complete;
    if (!window.complete) {
        window.end_offset_ns = window.start_offset_ns;
        for (GameBenchmarkFrameSample const& sample : frames) {
            if (sample.terrain_phase != window.phase
                || sample.started_offset_ns > std::numeric_limits<uint64_t>::max() - sample.loop_duration_ns) {
                continue;
            }
            uint64_t const completion_ns = sample.started_offset_ns + sample.loop_duration_ns;
            window.end_offset_ns = std::max(
                window.end_offset_ns,
                std::min(completion_ns, window.start_offset_ns + window.required_duration_ns)
            );
        }
        return window;
    }

    for (GameBenchmarkFrameCompletion const& completion : completions) {
        if (completion.submission_phase != window.phase
            || completion.submitted_offset_ns < window.start_offset_ns
            || completion.submitted_offset_ns > window.start_offset_ns + window.required_duration_ns) {
            continue;
        }
        window.end_offset_ns = std::max(window.end_offset_ns, completion.completion_observed_offset_ns);
    }
    return window;
}

std::optional<uint64_t> lastFrameCompletionOffset(std::span<GameBenchmarkFrameSample const> frames) noexcept
{
    if (frames.empty()) {
        return std::nullopt;
    }
    GameBenchmarkFrameSample const& last_frame = frames.back();
    if (last_frame.started_offset_ns > std::numeric_limits<uint64_t>::max() - last_frame.loop_duration_ns) {
        return std::nullopt;
    }
    return last_frame.started_offset_ns + last_frame.loop_duration_ns;
}

GameBenchmarkFrameBudgetWindow ordinaryFrameBudgetWindow(
    std::span<GameBenchmarkFrameSample const> frames,
    std::span<GameBenchmarkFrameCompletion const> completions,
    uint64_t const required_duration_ns
)
{
    GameBenchmarkFrameBudgetWindow window{};
    window.required_duration_ns = required_duration_ns;
    window.end_offset_ns = required_duration_ns;
    std::optional<uint64_t> const last_completion_offset = lastFrameCompletionOffset(frames);
    if (!last_completion_offset) {
        return window;
    }

    window.complete = *last_completion_offset >= window.end_offset_ns;
    if (!window.complete) {
        window.end_offset_ns = std::min(window.end_offset_ns, *last_completion_offset);
        return window;
    }
    for (GameBenchmarkFrameCompletion const& completion : completions) {
        if (completion.submission_phase != window.phase
            || completion.submitted_offset_ns > window.end_offset_ns) {
            continue;
        }
        window.end_offset_ns = std::max(window.end_offset_ns, completion.completion_observed_offset_ns);
    }
    return window;
}

GameBenchmarkFullTerrainSnapshot fullTerrainSnapshot(
    client::PlayerClientBenchmarkFullTerrainInfo const& info,
    client::StoneFaceGrowthDiagnostics const& growth,
    client::WorldLoadingStatus const& world_loading,
    GameBenchmarkFullRadiusPhase const phase,
    Clock::duration const elapsed
)
{
    GameBenchmarkFullTerrainSnapshot snapshot{
        .phase = std::string{ fullRadiusPhaseName(phase) },
        .phase_elapsed_ms = positiveNanoseconds(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
        ) / 1'000'000U,
        .scan_performed = info.scan_performed,
        .potentially_ready = info.potentially_ready,
        .exact_current_coverage = info.exact_current_coverage,
        .world_loading_state = std::string{worldLoadingStateName(world_loading.state)},
        .playable_sphere_radius_blocks = shared::MAX_SPHERE_RADIUS_BLOCKS,
        .playable_sphere_ready_chunks = world_loading.playable_sphere_ready_chunks,
        .playable_sphere_required_chunks = world_loading.playable_sphere_required_chunks,
        .playable_sphere_ready = world_loading.playable_sphere_ready,
        .center_valid = info.center.has_value(),
        .desired_count = info.desired_count,
        .resident_total_count = info.resident_total_count,
        .visible_mesh_count = info.visible_mesh_count,
        .renderer_mesh_count = info.renderer_mesh_count,
        .renderer_face_count = info.renderer_face_count,
        .renderer_draw_face_count = info.renderer_draw_face_count,
        .renderer_stone_face_capacity = info.renderer_stone_face_capacity,
        .renderer_stone_face_growth = growth,
        .pending_delivery_batch_count = info.pending_delivery_batch_count,
        .pending_residency_change_count = info.pending_residency_change_count,
        .pending_mesh_count = info.pending_mesh_count,
        .active_mesh_job_count = info.active_mesh_job_count,
        .dirty_mesh_job_count = info.dirty_mesh_job_count,
        .worker_result_count = info.worker_result_count,
        .worker_outstanding_count = info.worker_outstanding_count,
        .pending_removal_count = info.pending_removal_count,
        .resident_desired_count = info.resident_desired_count,
        .nonresident_desired_count = info.nonresident_desired_count,
        .current_handle_mesh_visible_count = info.current_handle_mesh_visible_count,
        .stale_or_missing_mesh_count = info.stale_or_missing_mesh_count,
        .unclassified_resident_missing_count = info.unclassified_resident_missing_count,
        .visible_outside_interest_count = info.visible_outside_interest_count,
        .current_visible_mesh_detail_counts = info.current_visible_mesh_detail_counts,
        .unclassified_current_visible_mesh_detail_count = info.unclassified_current_visible_mesh_detail_count,
        .current_near_mesh_work_stages = GameBenchmarkMeshStageSummary{
            .client_pending_admission_count = info.client_pending_admission_count,
            .worker_queued_count = info.worker_queued_count,
            .worker_building_count = info.worker_building_count,
            .completed_result_waiting_count = info.completed_result_waiting_count,
            .oldest_client_pending_near_observed_age_ns = positiveNanoseconds(
                info.oldest_client_pending_near_observed_age
            ),
            .oldest_worker_queued_age_ns = positiveNanoseconds(info.oldest_worker_queued_age),
            .oldest_worker_building_age_ns = positiveNanoseconds(info.oldest_worker_building_age),
            .oldest_completed_result_waiting_age_ns = positiveNanoseconds(
                info.oldest_completed_result_waiting_age
            ),
        },
    };
    static_assert(client::PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_COUNT == 16U);
    for (uint32_t index = 0U; index < client::PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_COUNT; ++index) {
        auto const& source = info.radial_bands[index];
        snapshot.radial_bands[index] = GameBenchmarkFullTerrainSnapshot::RadialBand{
            .desired_count = source.desired_count,
            .resident_count = source.resident_count,
            .nonresident_count = source.nonresident_count,
            .current_mesh_visible_count = source.current_mesh_visible_count,
            .stale_or_missing_mesh_count = source.stale_or_missing_mesh_count,
        };
    }
    if (info.center) {
        snapshot.center_x = info.center->x;
        snapshot.center_y = info.center->y;
    }
    return snapshot;
}

std::optional<uint64_t> platformFrameDurationTarget(bool const qualification_proof) noexcept
{
#if defined(_WIN32)
    return qualification_proof ? 3'333'333U : 2'000'000U;
#elif defined(__APPLE__)
    static_cast<void>(qualification_proof);
    return 8'333'333U;
#else
    static_cast<void>(qualification_proof);
    return std::nullopt;
#endif
}

std::shared_ptr<server::WorldPreparation> startBenchmarkTerrainPreparation(
    shared::WorldConfiguration const& world_configuration,
    Clock::time_point const request_accepted_at,
    shared::corelang::Backend const backend
)
{
    generation::GenerationConfiguration generation_configuration{};
    generation_configuration.seed = world_configuration.seed;
    return std::make_shared<server::WorldPreparation>(
        generation_configuration, world_configuration.algorithm_version, request_accepted_at, backend
    );
}

void awaitBenchmarkTerrainPreparation(std::shared_ptr<server::WorldPreparation> const& preparation)
{
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 5 };
    std::optional<generation::GenerationStage> reported_stage;
    Clock::time_point last_report = Clock::now();
    for (;;) {
        server::WorldPreparationSnapshot const snapshot = preparation->latest();
        Clock::time_point const now = Clock::now();
        if (snapshot.latest_progress
            && (reported_stage != snapshot.latest_progress->progress.stage
                || now - last_report >= std::chrono::seconds{ 1 })) {
            generation::GenerationProgress const& progress = snapshot.latest_progress->progress;
            std::cout << "[benchmark-game] Terrain-v2 preparation stage "
                << static_cast<uint32_t>(progress.stage) << ' '
                << progress.completed << '/' << progress.total << '\n' << std::flush;
            reported_stage = progress.stage;
            last_report = now;
        }
        if (snapshot.state == server::WorldPreparationState::Ready) {
            if (!snapshot.ready_world) {
                throw std::runtime_error{ "Terrain-v2 benchmark preparation finished without a world" };
            }
            return;
        }
        if (snapshot.state == server::WorldPreparationState::Failed
            || snapshot.state == server::WorldPreparationState::Cancelled) {
            throw std::runtime_error{ "Terrain-v2 benchmark world preparation failed: " + snapshot.error };
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

constexpr std::string_view S1_MOVEMENT_FRAME_PHASE = "s1-x30-movement";

std::string_view s1FramePhaseAt(
    GameBenchmarkS1MovementWindow const& movement_window,
    Clock::time_point const sampled_at
) noexcept
{
    std::optional<Clock::time_point> const started_at = movement_window.startedAt();
    if (!started_at || sampled_at < *started_at) {
        return "s1-fill-ramp";
    }
    return sampled_at < *started_at + GAME_BENCHMARK_S1_MOVEMENT_WINDOW
        ? S1_MOVEMENT_FRAME_PHASE : "s1-post-window-tail";
}

GameBenchmarkFrameBudgetWindow s1FrameBudgetWindow(
    std::optional<Clock::time_point> const first_loop_at,
    std::optional<Clock::time_point> const movement_started_at,
    bool const movement_window_drained
)
{
    GameBenchmarkFrameBudgetWindow window{
        .required_duration_ns = positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
            GAME_BENCHMARK_S1_MOVEMENT_WINDOW
        )),
        .phase = S1_MOVEMENT_FRAME_PHASE,
        .complete = movement_window_drained,
        .completion_observation_tail_ns = positiveNanoseconds(
            std::chrono::duration_cast<std::chrono::nanoseconds>(shared::TICK)
        ),
    };
    if (!first_loop_at || !movement_started_at || *movement_started_at < *first_loop_at) {
        window.complete = false;
        return window;
    }

    window.start_offset_ns = positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
        *movement_started_at - *first_loop_at
    ));
    window.end_offset_ns = window.start_offset_ns + window.required_duration_ns;
    return window;
}

bool s1TerrainPerformanceComplete(GameBenchmarkEvidence& benchmark) noexcept
{
    if (!benchmark.s1_terrain_performance || !benchmark.controlled_start
        || !gameBenchmarkStartActionComplete(*benchmark.controlled_start)) {
        return false;
    }
    GameBenchmarkS1TerrainPerformanceEvidence& evidence = *benchmark.s1_terrain_performance;
    shared::TerrainIdentity const canonical_identity{
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = 1U,
        .seed = shared::WorldConfiguration::SEED,
        .configuration_identity = generation::configurationIdentity(generation::GenerationConfiguration{}),
    };
    if (evidence.actual_radius_blocks != shared::ACTUAL_TERRAIN_RADIUS_BLOCKS
        || evidence.visual_lod_radius_blocks != shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS
        || benchmark.terrain_identity != canonical_identity || benchmark.session_join_epoch == 0U) {
        return false;
    }
    uint64_t const server_tick_ns = positiveNanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(shared::TICK)
    );

    auto const currentVisual = [&benchmark](
        std::optional<GameBenchmarkFrameSample::VisualLod> const& visual,
        GameBenchmarkFrameSample::PlayableSphere const& sphere
    ) {
        return gameBenchmarkS1VisualLodSampleReady(visual)
            && benchmark.terrain_identity == visual->terrain_identity
            && benchmark.session_join_epoch == visual->session_join_epoch
            && sphere.active_coverage_epoch && visual->coverage_epoch <= *sphere.active_coverage_epoch
            && sphere.world_loading_center
            && samePlayerPosition(*visual->coverage_center, *sphere.world_loading_center);
    };
    bool movement_loop_spheres_complete = true;
    bool movement_visual_lod_samples_complete = true;
    bool movement_frame_observed = false;
    bool movement_settings_complete = true;
    for (GameBenchmarkFrameSample const& frame : benchmark.raw_frames) {
        if (frame.terrain_phase != S1_MOVEMENT_FRAME_PHASE) {
            continue;
        }
        movement_frame_observed = true;
#if defined(_WIN32)
        constexpr uint32_t EXPECTED_WIDTH = 1'280U;
        constexpr uint32_t EXPECTED_HEIGHT = 720U;
#else
        constexpr uint32_t EXPECTED_WIDTH = 2'560U;
        constexpr uint32_t EXPECTED_HEIGHT = 1'440U;
#endif
        auto const& settings = frame.qualification_settings;
        movement_settings_complete = movement_settings_complete && settings
            && settings->framebuffer_width == EXPECTED_WIDTH && settings->framebuffer_height == EXPECTED_HEIGHT
            && settings->render_distance == 256U && settings->debug_hud_enabled
            && settings->terrain_model == shared::TERRAIN_LOD_MODEL
            && settings->camera_perspective == "first-person" && settings->world_mode == "flight"
            && frame.diagnostic_camera[6] == 70.0;
        if (!frame.playable_sphere || !gameBenchmarkS1CurrentTerrainSampleReady(*frame.playable_sphere)) {
            movement_loop_spheres_complete = false;
        }
        if (!frame.playable_sphere || !currentVisual(frame.visual_lod, *frame.playable_sphere)) {
            movement_visual_lod_samples_complete = false;
        }
    }
    movement_visual_lod_samples_complete = movement_visual_lod_samples_complete && movement_frame_observed;

    bool movement_authoritative_spheres_complete =
        gameBenchmarkS1MovementReceiptsComplete(benchmark)
        && evidence.movement_authoritative_update_count
            == evidence.movement_authoritative_sphere_updates.size()
        && evidence.movement_boundary_authoritative_updates_complete
        && evidence.movement_trajectory_anchor_sphere_sample_offset_ns.has_value();
    auto const hasCurrentSphere = [](GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& update) {
        return gameBenchmarkS1CurrentTerrainSampleReady(update.playable_sphere)
            && samePlayerPosition(
                update.authoritative_position,
                *update.playable_sphere.world_loading_center
            );
    };
    if (!evidence.movement_trajectory_anchor || !hasCurrentSphere(*evidence.movement_trajectory_anchor)) {
        movement_authoritative_spheres_complete = false;
    }
    for (GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& update
        : evidence.movement_authoritative_sphere_updates) {
        if (!hasCurrentSphere(update)) {
            movement_authoritative_spheres_complete = false;
        }
        if (!currentVisual(update.visual_lod, update.playable_sphere)) {
            movement_visual_lod_samples_complete = false;
        }
    }
    for (GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& update
        : evidence.movement_boundary_authoritative_updates) {
        if (!hasCurrentSphere(update)) {
            movement_authoritative_spheres_complete = false;
        }
        if (!currentVisual(update.visual_lod, update.playable_sphere)) {
            movement_visual_lod_samples_complete = false;
        }
    }
    if (!evidence.movement_trajectory_anchor
        || !currentVisual(evidence.movement_trajectory_anchor->visual_lod,
            evidence.movement_trajectory_anchor->playable_sphere)) {
        movement_visual_lod_samples_complete = false;
    }
    evidence.movement_playable_sphere_complete = movement_frame_observed && movement_loop_spheres_complete
        && movement_authoritative_spheres_complete;
    evidence.movement_visual_lod_samples_complete = movement_visual_lod_samples_complete;

    std::optional<uint64_t> const ready_prompt_offset = evidence.ready_prompt_frame_offset_ns;
    std::optional<uint64_t> const playable_ready_offset = evidence.playable_sphere_first_ready_offset_ns;
    std::optional<uint64_t> const start_ack_offset =
        benchmark.controlled_start->server_start_acknowledged_offset_ns;
    if (!ready_prompt_offset || !playable_ready_offset || !start_ack_offset
        || !gameBenchmarkS1ReadinessDeadlinesMet(ready_prompt_offset, playable_ready_offset)
        || benchmark.controlled_start->eligible_prompt_rendered_offset_ns != ready_prompt_offset
        || evidence.initial_deadline_missed || evidence.playable_deadline_missed) {
        return false;
    }

    uint64_t const first_eligible_movement_offset = std::max(*playable_ready_offset, *start_ack_offset);
    bool has_fill_input = false;
    bool fill_inputs_have_valid_profile = true;
    std::optional<uint64_t> first_post_ready_movement_offset;
    for (GameBenchmarkRawInputEvidence const& input : benchmark.raw_gameplay_inputs) {
        if (input.sent_offset_ns > std::numeric_limits<uint64_t>::max() - *start_ack_offset) {
            return false;
        }
        uint64_t const sent_t0_offset = *start_ack_offset + input.sent_offset_ns;
        bool const has_movement_intent = input.movement_intent.strafe != 0
            || input.movement_intent.forward != 0
            || input.movement_intent.vertical != 0;
        bool const has_movement_direction = input.direction.x != 0U
            || input.direction.y != 0U
            || input.direction.z != 0U;
        bool const movement_input = has_movement_intent && has_movement_direction;
        bool const any_movement_input = has_movement_intent || has_movement_direction;
        bool const x30_movement_input = movement_input && isX30MovementInput(input.direction);
        bool const valid_s1_movement_profile = movement_input && input.direction.speedup == 30U;
        if (sent_t0_offset >= *start_ack_offset && sent_t0_offset < *playable_ready_offset) {
            has_fill_input = has_fill_input || any_movement_input;
            fill_inputs_have_valid_profile = fill_inputs_have_valid_profile
                && (!any_movement_input || valid_s1_movement_profile);
        }
        if (sent_t0_offset >= first_eligible_movement_offset && x30_movement_input
            && (!first_post_ready_movement_offset || sent_t0_offset < *first_post_ready_movement_offset)) {
            first_post_ready_movement_offset = sent_t0_offset;
        }
    }
    uint64_t const start_t0_offset = evidence.movement_start_offset_ns;
    bool const ready_before_movement = evidence.movement_started_after_current_sphere_ready
        && start_t0_offset >= first_eligible_movement_offset
        && start_t0_offset - first_eligible_movement_offset <= server_tick_ns
        && first_post_ready_movement_offset == start_t0_offset;
    bool const fill_window_exercised = *playable_ready_offset <= *start_ack_offset
        || *playable_ready_offset - *start_ack_offset <= server_tick_ns
        || has_fill_input;
    bool const fill_evidence_consistent = evidence.movement_input_during_fill == has_fill_input
        && fill_inputs_have_valid_profile
        && fill_window_exercised;
    uint64_t const movement_duration_ns = positiveNanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_S1_MOVEMENT_WINDOW)
    );
    bool const exact_movement_window = start_t0_offset >= *start_ack_offset
        && evidence.movement_end_offset_ns == start_t0_offset + movement_duration_ns;
    bool const complete_input_receipt = !benchmark.raw_gameplay_inputs.empty()
        && benchmark.inputs_sent == benchmark.raw_gameplay_inputs.size();

    bool frame_budget_complete = false;
    if (benchmark.frame_budget && start_t0_offset >= *start_ack_offset) {
        GameBenchmarkFrameBudgetEvidence const& budget = *benchmark.frame_budget;
        uint64_t const expected_start_offset = start_t0_offset - *start_ack_offset;
        frame_budget_complete = budget.measurement_window_complete
            && budget.required_window_duration_ns == movement_duration_ns
            && budget.measurement_window_start_offset_ns == expected_start_offset
            && budget.measurement_window_end_offset_ns == expected_start_offset + movement_duration_ns
            && budget.measured_time_ns == movement_duration_ns;
        if (evidence.movement_trajectory_anchor) {
            uint64_t const anchor_offset = evidence.movement_trajectory_anchor->offset_ns;
            frame_budget_complete = frame_budget_complete
                && evidence.movement_trajectory_anchor_sphere_sample_offset_ns == anchor_offset
                && anchor_offset < expected_start_offset
                && expected_start_offset - anchor_offset <= server_tick_ns;
        } else {
            frame_budget_complete = false;
        }
    }

    evidence.complete = evidence.movement_playable_sphere_complete
        && evidence.movement_visual_lod_samples_complete
        && movement_settings_complete
        && gameBenchmarkS1RenderedCameraTrajectoryComplete(benchmark.raw_frames)
        && ready_before_movement
        && fill_evidence_consistent
        && exact_movement_window
        && complete_input_receipt
        && frame_budget_complete;
    return evidence.complete;
}

} // namespace

uint32_t gameBenchmarkS1PresentedCurrentColumnCount(
    client::TerrainLodScene const& scene,
    shared::PlayerPosition const center
)
{
    constexpr uint32_t DIAMETER = 81U;
    constexpr uint32_t PERIOD = shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH;
    uint32_t const center_x = shared::WorldBounds::wrapHorizontal(center.x) / shared::Chunk::SIDE_LENGTH;
    uint32_t const center_y = shared::WorldBounds::wrapHorizontal(center.y) / shared::Chunk::SIDE_LENGTH;
    std::bitset<static_cast<uint64_t>(DIAMETER) * DIAMETER> included;
    std::bitset<static_cast<uint64_t>(DIAMETER) * DIAMETER> backed;
    for (auto const& source : scene.authority_sources) {
        if (source.identity != scene.identity || source.coordinate.x < 0 || source.coordinate.y < 0
            || static_cast<uint32_t>(source.coordinate.x) >= PERIOD
            || static_cast<uint32_t>(source.coordinate.y) >= PERIOD || source.coordinate.z != 0) {
            continue;
        }
        uint32_t const x = (static_cast<uint32_t>(source.coordinate.x) + PERIOD - center_x + 40U) % PERIOD;
        uint32_t const y = (static_cast<uint32_t>(source.coordinate.y) + PERIOD - center_y + 40U) % PERIOD;
        if (x < DIAMETER && y < DIAMETER) {
            backed.set(y * DIAMETER + x);
        }
    }
    for (auto const& leaf : scene.leaves) {
        if (leaf.level != shared::TerrainLodLevel::L3 || leaf.x >= PERIOD || leaf.y >= PERIOD) {
            continue;
        }
        uint32_t const x = (leaf.x + PERIOD - center_x + 40U) % PERIOD;
        uint32_t const y = (leaf.y + PERIOD - center_y + 40U) % PERIOD;
        if (x < DIAMETER && y < DIAMETER) {
            included.set(y * DIAMETER + x);
        }
    }
    return static_cast<uint32_t>((included & backed).count());
}

bool gameBenchmarkS1RenderedCameraTrajectoryComplete(
    std::span<GameBenchmarkFrameSample const> const frames
) noexcept
{
    constexpr double BODY_CENTER_OFFSET = static_cast<double>(shared::World::PLAYER_WIDTH_SUBCELLS)
        / shared::SUBCELLS_PER_CELL / 2.0;
    constexpr double MAXIMUM_ATTACHMENT_LAG = 2.0 * shared::MOVEMENT_SUBCELLS_PER_TICK * 30.0
        / shared::SUBCELLS_PER_CELL;
    auto const wrappedDelta = [](double const current, double const previous) noexcept {
        return std::remainder(current - previous, static_cast<double>(shared::WorldExtent::WIDTH));
    };
    auto const distance = [&wrappedDelta](std::array<double, 3U> const& current,
                              std::array<double, 3U> const& previous) noexcept {
        double const x = wrappedDelta(current[0], previous[0]);
        double const y = wrappedDelta(current[1], previous[1]);
        double const z = current[2] - previous[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    std::optional<std::array<double, 3U>> previous_camera;
    std::array<double, 3U> previous_authority{};
    double rendered_travel = 0.0;
    double authoritative_travel = 0.0;
    for (auto const& frame : frames) {
        if (frame.terrain_phase != S1_MOVEMENT_FRAME_PHASE || !frame.presentation_succeeded) {
            continue;
        }
        if (!frame.playable_sphere || !frame.playable_sphere->world_loading_center
            || !frame.presented_player
            || !std::ranges::all_of(frame.presented_player->body, [](double const value) { return std::isfinite(value); })
            || frame.presented_player->sampled_offset_ns < frame.started_offset_ns
            || frame.presented_player->sampled_offset_ns > frame.started_offset_ns + frame.loop_duration_ns
            || !frame.visual_lod
            || frame.presented_player->submitted_frame_count != frame.visual_lod->submitted_frame_count
            || !std::ranges::all_of(frame.diagnostic_camera, [](double const value) { return std::isfinite(value); })) {
            return false;
        }
        auto const position = *frame.playable_sphere->world_loading_center;
        std::array<double, 3U> const authority{
            static_cast<double>(position.x) + static_cast<double>(position.x_subcell) / shared::SUBCELLS_PER_CELL,
            static_cast<double>(position.y) + static_cast<double>(position.y_subcell) / shared::SUBCELLS_PER_CELL,
            static_cast<double>(position.z) + static_cast<double>(position.z_subcell) / shared::SUBCELLS_PER_CELL,
        };
        std::array<double, 3U> const camera{
            frame.diagnostic_camera[0] - BODY_CENTER_OFFSET,
            frame.diagnostic_camera[1] - BODY_CENTER_OFFSET,
            frame.diagnostic_camera[2] - client::PLAYER_EYE_HEIGHT,
        };
        constexpr double MINIMUM_CAMERA_XY = -client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
        constexpr double MAXIMUM_CAMERA_XY = static_cast<double>(shared::WorldExtent::WIDTH)
            + client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
        if (camera[0] < MINIMUM_CAMERA_XY || camera[0] > MAXIMUM_CAMERA_XY
            || camera[1] < MINIMUM_CAMERA_XY || camera[1] > MAXIMUM_CAMERA_XY
            || distance(camera, authority) > MAXIMUM_ATTACHMENT_LAG + 1e-6
            || distance(camera, frame.presented_player->body) > 1e-4) {
            return false;
        }
        if (previous_camera) {
            rendered_travel += distance(camera, *previous_camera);
            authoritative_travel += distance(authority, previous_authority);
        }
        previous_camera = camera;
        previous_authority = authority;
    }
    return rendered_travel > 0.0 && authoritative_travel > 0.0
        && rendered_travel >= authoritative_travel * 0.9;
}

bool gameBenchmarkS1MovementReceiptsComplete(GameBenchmarkEvidence const& benchmark) noexcept
{
    if (!benchmark.s1_terrain_performance || !benchmark.frame_budget) {
        return false;
    }
    auto const& evidence = *benchmark.s1_terrain_performance;
    auto const& budget = *benchmark.frame_budget;
    uint64_t const start = budget.measurement_window_start_offset_ns;
    uint64_t const end = budget.measurement_window_end_offset_ns;
    uint64_t const tick_ns = positiveNanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(shared::TICK)
    );
    uint64_t const duration_ns = positiveNanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(GAME_BENCHMARK_S1_MOVEMENT_WINDOW)
    );
    if (end < start || end - start != duration_ns
        || end > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - tick_ns
        || evidence.movement_authoritative_update_count != evidence.movement_authoritative_sphere_updates.size()) {
        return false;
    }
    uint32_t input_count = 0U;
    uint32_t previous_sequence = 0U;
    uint64_t previous_ack_offset = 0U;
    int64_t previous_schedule = 0;
    for (GameBenchmarkRawInputEvidence const& input : benchmark.raw_gameplay_inputs) {
        if (input.sent_offset_ns < start || input.sent_offset_ns >= end) {
            continue;
        }
        int64_t const sent = static_cast<int64_t>(input.sent_offset_ns);
        if (input_count >= evidence.movement_authoritative_sphere_updates.size()
            || input.scheduled_offset_ns > sent || input.scheduled_offset_ns <= sent - static_cast<int64_t>(tick_ns)
            || (input_count == 0U && input.sent_offset_ns != start)
            || (input_count != 0U && (input.sequence != previous_sequence + 1U
                || input.scheduled_offset_ns != previous_schedule + static_cast<int64_t>(tick_ns)))) {
            return false;
        }
        auto const& update = evidence.movement_authoritative_sphere_updates[input_count];
        if (update.acknowledged_input_sequence != input.sequence
            || update.offset_ns < input.sent_offset_ns || update.offset_ns > input.sent_offset_ns + tick_ns
            || update.offset_ns >= end + tick_ns
            || (input_count != 0U && update.offset_ns <= previous_ack_offset)) {
            return false;
        }
        previous_sequence = input.sequence;
        previous_schedule = input.scheduled_offset_ns;
        previous_ack_offset = update.offset_ns;
        ++input_count;
    }
    return input_count >= GAME_BENCHMARK_S1_EXPECTED_MOVEMENT_UPDATES
        && input_count == evidence.movement_authoritative_sphere_updates.size()
        && previous_schedule >= static_cast<int64_t>(end - 2U * tick_ns);
}

bool gameBenchmarkS1VisualLodSampleReady(
    std::optional<GameBenchmarkFrameSample::VisualLod> const& sample
) noexcept
{
    return sample && sample->radius_blocks == shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS
        && sample->scene_active && sample->leaf_count >= 6'561U
        && sample->actual_column_count >= 6'561U && sample->leaf_count >= sample->actual_column_count
        && sample->terrain_identity
        && !sample->terrain_identity->configuration_identity.empty()
        && sample->session_join_epoch != 0U && sample->coverage_epoch != 0U
        && sample->submitted_frame_count != 0U
        && sample->scene_center_x < shared::WorldExtent::WIDTH
        && sample->scene_center_y < shared::WorldExtent::WIDTH
        && sample->actual_radius_blocks == shared::ACTUAL_TERRAIN_RADIUS_BLOCKS
        && sample->required_actual_columns == 6'561U
        && sample->presented_current_actual_columns == 6'561U
        && sample->coverage_center && sample->sources_current;
}

bool gameBenchmarkS1ReadinessDeadlinesMet(
    std::optional<uint64_t> const ready_prompt_offset_ns,
    std::optional<uint64_t> const playable_ready_offset_ns
) noexcept
{
#if defined(_WIN32)
    constexpr uint64_t PLAYABLE_SPHERE_DEADLINE_NS = 15'000'000'000U;
#else
    constexpr uint64_t PLAYABLE_SPHERE_DEADLINE_NS = 20'000'000'000U;
#endif
    return ready_prompt_offset_ns && playable_ready_offset_ns
        && *ready_prompt_offset_ns <= 4'000'000'000U
        && *playable_ready_offset_ns <= PLAYABLE_SPHERE_DEADLINE_NS;
}

std::optional<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
gameBenchmarkS1TrajectoryAnchorSample(
    GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& authoritative_state,
    GameBenchmarkFrameSample::PlayableSphere const& current_sphere,
    GameBenchmarkFrameSample::VisualLod const& visual_lod,
    uint64_t const sampled_offset_ns
)
{
    if (!gameBenchmarkS1CurrentTerrainSampleReady(current_sphere)
        || !current_sphere.world_loading_center.has_value()
        || !samePlayerPosition(authoritative_state.authoritative_position, *current_sphere.world_loading_center)) {
        return std::nullopt;
    }
    auto anchor = authoritative_state;
    anchor.offset_ns = sampled_offset_ns;
    anchor.playable_sphere = current_sphere;
    anchor.visual_lod = visual_lod;
    return anchor;
}

void gameBenchmarkObserveGeometricSphere(
    GameBenchmarkFrameSample::PlayableSphere& sample,
    shared::SparseWorld const& world
)
{
    sample.ready = false;
    sample.ready_chunks = 0U;
    sample.required_chunks = 0U;
    if (!sample.world_loading_center || !sample.coverage_scope.empty()) {
        return;
    }
    thread_local std::optional<shared::PlayerPosition> cached_center;
    thread_local uint32_t cached_radius = 0U;
    thread_local std::vector<shared::ChunkCoordinate> cached_keys;
    if (!cached_center || !samePlayerPosition(*cached_center, *sample.world_loading_center)
        || cached_radius != sample.radius_blocks) {
        cached_center = sample.world_loading_center;
        cached_radius = sample.radius_blocks;
        cached_keys = shared::requiredChunkKeys(*cached_center, cached_radius);
    }
    sample.required_chunks = static_cast<uint32_t>(cached_keys.size());
    for (shared::ChunkCoordinate const key : cached_keys) {
        if (world.hasAuthoritativeChunk(key)) {
            ++sample.ready_chunks;
        }
    }
    sample.ready = sample.required_chunks != 0U && sample.ready_chunks == sample.required_chunks;
}

bool gameBenchmarkS1CurrentTerrainSampleReady(
    GameBenchmarkFrameSample::PlayableSphere const& sample
) noexcept
{
    constexpr uint32_t COLUMNS_PER_SIDE = 2U * shared::FULL_DETAIL_RADIUS_CHUNKS + 1U;
    constexpr uint32_t REQUIRED_COLUMNS = COLUMNS_PER_SIDE * COLUMNS_PER_SIDE;
    constexpr uint32_t VERTICAL_CHUNKS = (shared::World::FLIGHT_MAX_Z + 1U) / shared::Chunk::SIDE_LENGTH;
    return playableSphereSampleReady(sample, shared::ACTUAL_TERRAIN_RADIUS_BLOCKS)
        && sample.active_coverage_epoch.has_value()
        && *sample.active_coverage_epoch != 0U
        && sample.coverage_scope == "horizontal_full_volume"
        && sample.required_authoritative_columns == REQUIRED_COLUMNS
        && sample.resident_authoritative_columns == REQUIRED_COLUMNS
        && sample.vertical_chunk_count == VERTICAL_CHUNKS
        && sample.required_chunks == REQUIRED_COLUMNS * VERTICAL_CHUNKS
        && sample.authoritative_columns_center.has_value()
        && sample.world_loading_center.has_value()
        && sample.authoritative_columns_center->x / shared::Chunk::SIDE_LENGTH
            == sample.world_loading_center->x / shared::Chunk::SIDE_LENGTH
        && sample.authoritative_columns_center->y / shared::Chunk::SIDE_LENGTH
            == sample.world_loading_center->y / shared::Chunk::SIDE_LENGTH;
}

bool GameBenchmarkFrameCompletionTracker::track(GameBenchmarkPendingFrameSubmission submission)
{
    if (submission.sequence_id == 0U || submission.frame_slot >= m_pending.size()
        || m_pending[submission.frame_slot]) {
        return false;
    }
    m_pending[submission.frame_slot] = std::move(submission);
    return true;
}

void GameBenchmarkFrameCompletionTracker::retireOtherGenerations(
    uint64_t const current_swapchain_generation
) noexcept
{
    for (std::optional<GameBenchmarkPendingFrameSubmission>& pending : m_pending) {
        if (pending && pending->swapchain_generation != current_swapchain_generation) {
            pending.reset();
        }
    }
}

std::optional<GameBenchmarkFrameCompletion> GameBenchmarkFrameCompletionTracker::confirmFence(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): fields follow the fence observation contract.
    uint64_t const observed_swapchain_generation,
    uint32_t const observed_frame_slot,
    uint64_t const completion_observed_offset_ns
)
{
    if (observed_frame_slot >= m_pending.size()) {
        return std::nullopt;
    }
    std::optional<GameBenchmarkPendingFrameSubmission>& pending = m_pending[observed_frame_slot];
    if (!pending) {
        return std::nullopt;
    }
    if (pending->swapchain_generation != observed_swapchain_generation) {
        return std::nullopt;
    }
    GameBenchmarkPendingFrameSubmission const submission = std::move(pending.value());
    pending.reset();
    return GameBenchmarkFrameCompletion{
        .sequence_id = submission.sequence_id,
        .swapchain_generation = submission.swapchain_generation,
        .frame_slot = submission.frame_slot,
        .submitted_offset_ns = submission.submitted_offset_ns,
        .completion_observed_offset_ns = completion_observed_offset_ns,
        .application_present_id = submission.application_present_id,
        .submission_phase = submission.submission_phase,
    };
}

bool GameBenchmarkFrameCompletionTracker::hasPendingPhase(std::string_view const phase) const noexcept
{
    return std::ranges::any_of(m_pending, [phase](
        std::optional<GameBenchmarkPendingFrameSubmission> const& pending
    ) {
        return pending && pending->submission_phase == phase;
    });
}

bool GameBenchmarkS1MovementWindow::observeSuccessfulInput(
    std::chrono::steady_clock::time_point const sent_at,
    bool const playable_sphere_ready
) noexcept
{
    if (m_started_at || !playable_sphere_ready) {
        return false;
    }
    m_started_at = sent_at;
    return true;
}

bool GameBenchmarkS1MovementWindow::shouldPauseBeforeStart(
    bool const playable_sphere_ready_observed,
    bool const current_sphere_ready,
    bool const trajectory_anchor_ready
) const noexcept
{
    return !m_started_at.has_value()
        && playable_sphere_ready_observed
        && (!current_sphere_ready || !trajectory_anchor_ready);
}

std::chrono::nanoseconds GameBenchmarkS1MovementWindow::routeElapsedAt(
    std::chrono::steady_clock::time_point const sampled_at,
    std::optional<std::chrono::steady_clock::time_point> const fill_route_started_at,
    bool const playable_sphere_ready
) const noexcept
{
    std::chrono::steady_clock::time_point route_started_at = sampled_at;
    if (m_started_at) {
        route_started_at = *m_started_at;
    } else if (!playable_sphere_ready && fill_route_started_at) {
        route_started_at = *fill_route_started_at;
    }
    if (sampled_at <= route_started_at) {
        return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sampled_at - route_started_at);
}

std::optional<std::chrono::steady_clock::time_point> GameBenchmarkS1MovementWindow::startedAt() const noexcept
{
    return m_started_at;
}

bool GameBenchmarkS1MovementWindow::contains(
    std::chrono::steady_clock::time_point const sampled_at
) const noexcept
{
    return m_started_at && sampled_at >= *m_started_at
        && sampled_at < *m_started_at + GAME_BENCHMARK_S1_MOVEMENT_WINDOW;
}

shared::Direction gameBenchmarkS1MovementDirection(
    GameBenchmarkMovementRouteSample const& route,
    bool const playable_sphere_ready
) noexcept
{
    shared::Direction direction = gameBenchmarkDirection(route);
    direction.accelerated = playable_sphere_ready;
    return direction;
}

GameBenchmarkFullRadiusSequence::GameBenchmarkFullRadiusSequence(
    std::chrono::seconds const warmup_timeout,
    bool const defer_movement_window_until_input
) noexcept
    : m_warmup_timeout(warmup_timeout)
    , m_defer_movement_window_until_input(defer_movement_window_until_input)
{ }

void GameBenchmarkFullRadiusSequence::start(Clock::time_point const started_at) noexcept
{
    m_started = true;
    m_phase_started_at = started_at;
}

void GameBenchmarkFullRadiusSequence::observe(
    Clock::time_point const sampled_at,
    std::optional<bool> const exact_current_coverage,
    bool const actual_playable_sphere_ready
) noexcept
{
    if (!m_started) {
        start(sampled_at);
        return;
    }
    if (m_phase == GameBenchmarkFullRadiusPhase::Complete || m_phase == GameBenchmarkFullRadiusPhase::Failed) {
        return;
    }
    if (m_phase == GameBenchmarkFullRadiusPhase::Movement
        && m_defer_movement_window_until_input && !m_movement_window_started) {
        return;
    }

    std::chrono::seconds phase_duration{};
    switch (m_phase) {
        case GameBenchmarkFullRadiusPhase::InitialWarmup:
        case GameBenchmarkFullRadiusPhase::RecoveryWarmup:
            if (phaseElapsed(sampled_at) > m_warmup_timeout) {
                transition(GameBenchmarkFullRadiusPhase::Failed, sampled_at);
                return;
            }
            if (exact_current_coverage.value_or(false) && actual_playable_sphere_ready) {
                transition(
                    m_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                        ? GameBenchmarkFullRadiusPhase::BaselineStationary
                        : GameBenchmarkFullRadiusPhase::RecoveredStationary,
                    sampled_at
                );
                return;
            }
            if (phaseElapsed(sampled_at) >= m_warmup_timeout) {
                transition(GameBenchmarkFullRadiusPhase::Failed, sampled_at);
            }
            return;
        case GameBenchmarkFullRadiusPhase::BaselineStationary:
        case GameBenchmarkFullRadiusPhase::RecoveredStationary:
            phase_duration = GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
            break;
        case GameBenchmarkFullRadiusPhase::Movement:
            phase_duration = GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
            break;
        case GameBenchmarkFullRadiusPhase::Complete:
        case GameBenchmarkFullRadiusPhase::Failed:
            return;
    }
    if (phaseElapsed(sampled_at) < phase_duration) {
        return;
    }
    switch (m_phase) {
        case GameBenchmarkFullRadiusPhase::BaselineStationary:
            transition(GameBenchmarkFullRadiusPhase::Movement, sampled_at);
            break;
        case GameBenchmarkFullRadiusPhase::Movement:
            transition(GameBenchmarkFullRadiusPhase::RecoveryWarmup, sampled_at);
            break;
        case GameBenchmarkFullRadiusPhase::RecoveredStationary:
            transition(GameBenchmarkFullRadiusPhase::Complete, sampled_at);
            break;
        case GameBenchmarkFullRadiusPhase::InitialWarmup:
        case GameBenchmarkFullRadiusPhase::RecoveryWarmup:
        case GameBenchmarkFullRadiusPhase::Complete:
        case GameBenchmarkFullRadiusPhase::Failed:
            break;
    }
}

void GameBenchmarkFullRadiusSequence::beginMovementWindow(Clock::time_point const started_at) noexcept
{
    if (m_phase != GameBenchmarkFullRadiusPhase::Movement
        || !m_defer_movement_window_until_input || m_movement_window_started) {
        return;
    }
    m_phase_started_at = started_at;
    m_movement_window_started = true;
}

std::chrono::steady_clock::duration GameBenchmarkFullRadiusSequence::phaseElapsed(
    Clock::time_point const sampled_at
) const noexcept
{
    if (m_phase == GameBenchmarkFullRadiusPhase::Movement
        && m_defer_movement_window_until_input && !m_movement_window_started) {
        return Clock::duration::zero();
    }
    return sampled_at > m_phase_started_at ? sampled_at - m_phase_started_at : Clock::duration::zero();
}

bool gameBenchmarkRecoveryIsCloserToMovementEnd(
    shared::HeightTileKey const movement_start,
    shared::HeightTileKey const movement_end,
    shared::HeightTileKey const recovery_center
) noexcept
{
    static constexpr int32_t WORLD_TILE_COUNT = static_cast<int32_t>(
        shared::World::FLIGHT_MAX_CELL / shared::Chunk::SIDE_LENGTH
    );
    auto const wrapped_axis_distance = [](int32_t const first, int32_t const second) {
        int32_t normalized_first = first % WORLD_TILE_COUNT;
        int32_t normalized_second = second % WORLD_TILE_COUNT;
        normalized_first = normalized_first < 0 ? normalized_first + WORLD_TILE_COUNT : normalized_first;
        normalized_second = normalized_second < 0 ? normalized_second + WORLD_TILE_COUNT : normalized_second;
        int32_t const difference = normalized_first >= normalized_second
            ? normalized_first - normalized_second : normalized_second - normalized_first;
        return std::min(difference, WORLD_TILE_COUNT - difference);
    };
    auto const wrapped_tile_distance = [&](shared::HeightTileKey const first, shared::HeightTileKey const second) {
        return std::max(
            wrapped_axis_distance(first.x, second.x),
            wrapped_axis_distance(first.y, second.y)
        );
    };
    return wrapped_tile_distance(recovery_center, movement_end)
        < wrapped_tile_distance(recovery_center, movement_start);
}

void GameBenchmarkFullRadiusSequence::transition(
    GameBenchmarkFullRadiusPhase const phase,
    Clock::time_point const started_at
) noexcept
{
    m_phase = phase;
    m_phase_started_at = started_at;
    if (phase == GameBenchmarkFullRadiusPhase::Movement) {
        m_movement_window_started = !m_defer_movement_window_until_input;
    }
}

std::expected<std::chrono::seconds, std::string> gameBenchmarkFullRadiusWarmupTimeout(
    int64_t const seconds
)
{
    if (seconds <= 0 || seconds > GAME_BENCHMARK_FULL_RADIUS_MAX_WARMUP_TIMEOUT.count()) {
        return std::unexpected("full-radius warmup timeout must be from 1 to 1800 seconds");
    }
    return std::chrono::seconds{ seconds };
}

std::chrono::seconds gameBenchmarkDeadline(GameBenchmarkOptions const& options) noexcept
{
    if (options.workload != GameBenchmarkWorkload::Speed200FullRadiusRecovery) {
        return options.deadline;
    }
    int64_t const timeout = options.full_radius_warmup_timeout.count();
    if (timeout <= 0 || timeout > GAME_BENCHMARK_FULL_RADIUS_MAX_WARMUP_TIMEOUT.count()) {
        return options.deadline;
    }
    return options.full_radius_warmup_timeout * 2 + GAME_BENCHMARK_FULL_RADIUS_NON_WARMUP_BUDGET;
}

std::vector<std::chrono::nanoseconds> gameBenchmarkSuccessfulPresentIntervals(
    std::span<GameBenchmarkFrameSample const> const frames,
    std::string_view const stationary_phase
)
{
    std::vector<std::chrono::nanoseconds> intervals;
    intervals.reserve(frames.size());
    std::optional<uint64_t> previous_completion_ns;
    for (GameBenchmarkFrameSample const& sample : frames) {
        if (sample.terrain_phase != stationary_phase || !sample.presentation_succeeded) {
            continue;
        }
        uint64_t const completion_ns = sample.started_offset_ns + sample.loop_duration_ns;
        if (previous_completion_ns) {
            intervals.emplace_back(static_cast<int64_t>(completion_ns - *previous_completion_ns));
        }
        previous_completion_ns = completion_ns;
    }
    return intervals;
}

GameBenchmarkFrameBudgetEvidence gameBenchmarkFrameBudget(
    std::span<GameBenchmarkFrameSample const> const frames,
    std::span<GameBenchmarkFrameCompletion const> const fence_confirmed_frames,
    uint64_t const target_frame_duration_ns,
    GameBenchmarkFrameBudgetWindow const& window,
    uint32_t const required_in_budget_time_permille
)
{
    GameBenchmarkFrameBudgetEvidence evidence{
        .target_frame_duration_ns = target_frame_duration_ns,
        .required_in_budget_time_permille = required_in_budget_time_permille,
        .measurement_window_start_offset_ns = window.start_offset_ns,
        .measurement_window_end_offset_ns = window.end_offset_ns,
        .required_window_duration_ns = window.required_duration_ns,
    };
    if (target_frame_duration_ns == 0U || required_in_budget_time_permille > 1'000U
        || window.required_duration_ns == 0U || window.end_offset_ns < window.start_offset_ns
        || window.required_duration_ns > std::numeric_limits<uint64_t>::max() - window.start_offset_ns
        || window.completion_observation_tail_ns
            > std::numeric_limits<uint64_t>::max() - window.end_offset_ns) {
        return evidence;
    }

    evidence.measured_time_ns = window.end_offset_ns - window.start_offset_ns;
    evidence.measurement_window_complete = window.complete
        && evidence.measured_time_ns >= window.required_duration_ns;
    uint64_t const required_end_offset_ns = window.start_offset_ns + window.required_duration_ns;
    uint64_t const completion_observation_deadline_ns = window.end_offset_ns
        + window.completion_observation_tail_ns;
    uint64_t const submission_end_offset_ns = window.complete
        ? required_end_offset_ns
        : std::min(required_end_offset_ns, window.end_offset_ns);
    uint64_t previous_completion_ns = window.start_offset_ns;
    uint64_t previous_sequence_id = 0U;
    std::unordered_map<uint64_t, GameBenchmarkFrameSample const*> submitted_frames;
    submitted_frames.reserve(frames.size());

    for (GameBenchmarkFrameSample const& sample : frames) {
        if (!sample.presentation_succeeded || sample.submitted_frame_phase != window.phase) {
            continue;
        }
        if (sample.submitted_frame_offset_ns > submission_end_offset_ns
            || (window.completion_observation_tail_ns != 0U
                && sample.submitted_frame_offset_ns == submission_end_offset_ns)) {
            continue;
        }
        if (!sample.submitted_frame_sequence_id || !sample.swapchain_generation || !sample.submitted_frame_slot
            || *sample.submitted_frame_sequence_id == 0U
            || *sample.submitted_frame_slot >= 3U
            || sample.submitted_frame_offset_ns < window.start_offset_ns) {
            return evidence;
        }
        if ((sample.application_present_id && *sample.application_present_id == 0U)
            || !submitted_frames.emplace(*sample.submitted_frame_sequence_id, &sample).second) {
            return evidence;
        }
    }

    std::unordered_set<uint64_t> completed_sequence_ids;
    completed_sequence_ids.reserve(fence_confirmed_frames.size());
    for (GameBenchmarkFrameCompletion const& completion : fence_confirmed_frames) {
        if (completion.submission_phase != window.phase
            || completion.submitted_offset_ns < window.start_offset_ns
            || completion.submitted_offset_ns > submission_end_offset_ns
            || (window.completion_observation_tail_ns != 0U
                && completion.submitted_offset_ns == submission_end_offset_ns)
            || completion.completion_observed_offset_ns > completion_observation_deadline_ns) {
            continue;
        }
        auto const submitted = submitted_frames.find(completion.sequence_id);
        if (submitted == submitted_frames.end() || completion.sequence_id == 0U
            || completion.frame_slot >= 3U
            || (completion.application_present_id && *completion.application_present_id == 0U)
            || completion.completion_observed_offset_ns < window.start_offset_ns
            || !completed_sequence_ids.insert(completion.sequence_id).second) {
            return evidence;
        }
        GameBenchmarkFrameSample const& source = *submitted->second;
        if (source.swapchain_generation != completion.swapchain_generation
            || source.submitted_frame_slot != completion.frame_slot
            || source.submitted_frame_offset_ns != completion.submitted_offset_ns
            || source.application_present_id != completion.application_present_id
            || source.submitted_frame_sequence_id != completion.sequence_id
            || completion.submitted_offset_ns > completion.completion_observed_offset_ns
            || completion.sequence_id <= previous_sequence_id) {
            return evidence;
        }
        uint64_t const budget_completion_offset_ns = std::min(
            completion.completion_observed_offset_ns,
            window.end_offset_ns
        );
        if (budget_completion_offset_ns < previous_completion_ns) {
            return evidence;
        }
        uint64_t const interval_ns = budget_completion_offset_ns - previous_completion_ns;
        if (interval_ns <= target_frame_duration_ns) {
            evidence.in_budget_time_ns += interval_ns;
        }
        previous_completion_ns = budget_completion_offset_ns;
        previous_sequence_id = completion.sequence_id;
        ++evidence.fence_confirmed_frame_count;
        ++evidence.sample_count;
    }

    if (evidence.fence_confirmed_frame_count != submitted_frames.size()) {
        evidence.measurement_window_complete = false;
    }

    uint64_t const tail_ns = window.end_offset_ns - previous_completion_ns;
    if (tail_ns > 0U) {
        if (tail_ns <= target_frame_duration_ns) {
            evidence.in_budget_time_ns += tail_ns;
        }
        ++evidence.sample_count;
    }

    uint32_t const over_budget_permille = 1'000U - required_in_budget_time_permille;
    uint64_t const allowed_over_budget_time = (evidence.measured_time_ns / 1'000U) * over_budget_permille
        + ((evidence.measured_time_ns % 1'000U) * over_budget_permille) / 1'000U;
    uint64_t const measured_over_budget_time = evidence.measured_time_ns - evidence.in_budget_time_ns;
    evidence.passed = evidence.measurement_window_complete && evidence.fence_confirmed_frame_count != 0U
        && measured_over_budget_time <= allowed_over_budget_time;
    return evidence;
}

bool gameBenchmarkPresentationTimingComplete(GameBenchmarkPresentationTimingEvidence const& evidence)
{
    if (!evidence.capability_supported || !evidence.capture_endpoint || !evidence.drain_complete
        || evidence.successful_requests.empty() || evidence.polls.empty()) {
        return false;
    }

    std::unordered_set<PresentationRequestKey, PresentationRequestKeyHash> expected_requests;
    expected_requests.reserve(evidence.successful_requests.size());
    std::unordered_map<uint64_t, uint32_t> last_present_id_by_generation;
    last_present_id_by_generation.reserve(evidence.successful_requests.size());
    std::optional<uint64_t> previous_generation;
    std::optional<uint64_t> measurement_generation;
    std::optional<bool> s1_history;
    uint64_t previous_loop_ordinal = 0U;
    for (GameBenchmarkSuccessfulPresentationRequest const& request : evidence.successful_requests) {
        uint64_t const generation = request.request.swapchain_generation;
        uint32_t const present_id = request.request.application_present_id;
        bool const request_is_s1 = request.phase == "s1-fill-ramp" || request.phase == S1_MOVEMENT_FRAME_PHASE;
        bool const request_is_full_radius = request.phase == "initial-full-radius-warmup"
            || request.phase == "baseline-stationary" || request.phase == "full-radius-movement"
            || request.phase == "full-radius-recovery-warmup" || request.phase == "recovered-stationary";
        if (present_id == 0U || request.loop_ordinal <= previous_loop_ordinal
            || (previous_generation && generation < *previous_generation)
            || (!request_is_s1 && !request_is_full_radius)
            || (s1_history && *s1_history != request_is_s1)) {
            return false;
        }
        s1_history = request_is_s1;

        if (!measurement_generation) {
            if ((request_is_s1 && request.phase == S1_MOVEMENT_FRAME_PHASE)
                || (!request_is_s1 && request.phase == "baseline-stationary")) {
                if (previous_generation && generation != *previous_generation) {
                    return false;
                }
                measurement_generation = generation;
            } else if (request.phase != "initial-full-radius-warmup"
                && request.phase != "s1-fill-ramp") {
                return false;
            }
        } else if (generation != *measurement_generation
            || request.phase == "initial-full-radius-warmup"
            || request.phase == "s1-fill-ramp") {
            return false;
        }

        auto const last_id = last_present_id_by_generation.try_emplace(generation, 0U).first;
        if (present_id <= last_id->second) {
            return false;
        }
        last_id->second = present_id;

        if (!expected_requests.insert(presentationRequestKey(request.request)).second) {
            return false;
        }
        previous_generation = generation;
        previous_loop_ordinal = request.loop_ordinal;
    }
    if (!measurement_generation) {
        return false;
    }

    GameBenchmarkSuccessfulPresentationRequest const& last_request = evidence.successful_requests.back();
    if (evidence.capture_endpoint->request.swapchain_generation != *measurement_generation
        || evidence.capture_endpoint->request.application_present_id != last_request.request.application_present_id
        || last_request.request.swapchain_generation != *measurement_generation
        || evidence.capture_endpoint->loop_ordinal != last_request.loop_ordinal
        || (s1_history && *s1_history && last_request.phase != S1_MOVEMENT_FRAME_PHASE)) {
        return false;
    }

    std::unordered_set<PresentationRequestKey, PresentationRequestKeyHash> drain_requests;
    drain_requests.reserve(evidence.drain_request_ids.size());
    auto last_measurement_id = last_present_id_by_generation.find(*measurement_generation);
    if (last_measurement_id == last_present_id_by_generation.end()) {
        return false;
    }
    for (GameBenchmarkPresentationRequestId const& request : evidence.drain_request_ids) {
        PresentationRequestKey const key = presentationRequestKey(request);
        if (request.swapchain_generation != *measurement_generation
            || request.application_present_id == 0U
            || request.application_present_id <= last_measurement_id->second
            || expected_requests.contains(key)
            || !drain_requests.insert(key).second) {
            return false;
        }
        last_measurement_id->second = request.application_present_id;
    }

    uint64_t returned_count = 0U;
    bool drain_polls_started = false;
    for (GameBenchmarkPresentationPoll const& poll : evidence.polls) {
        if (poll.scope == GameBenchmarkPresentationPollScope::Drain) {
            drain_polls_started = true;
        } else if (poll.scope != GameBenchmarkPresentationPollScope::Loop || drain_polls_started) {
            return false;
        }
        if (poll.status == GameBenchmarkPresentationPollStatus::Unavailable
            || poll.status == GameBenchmarkPresentationPollStatus::Error) {
            return false;
        }
        if ((poll.status == GameBenchmarkPresentationPollStatus::Empty
                && (poll.vk_result != static_cast<int32_t>(VK_SUCCESS) || poll.returned_count != 0U))
            || (poll.status == GameBenchmarkPresentationPollStatus::Records
                && (poll.vk_result != static_cast<int32_t>(VK_SUCCESS) || poll.returned_count == 0U))
            || (poll.status == GameBenchmarkPresentationPollStatus::Incomplete
                && poll.vk_result != static_cast<int32_t>(VK_SUCCESS)
                && poll.vk_result != static_cast<int32_t>(VK_INCOMPLETE))) {
            return false;
        }
        returned_count += poll.returned_count;
    }
    if (!drain_polls_started || returned_count != evidence.records.size()) {
        return false;
    }

    std::unordered_map<PresentationRequestKey, uint64_t, PresentationRequestKeyHash> times_by_request;
    times_by_request.reserve(evidence.records.size());
    for (GameBenchmarkPresentationTimingRecord const& record : evidence.records) {
        PresentationRequestKey const key = presentationRequestKey(record.request);
        if (record.request.application_present_id == 0U || record.actual_present_time_ns == 0U
            || (!expected_requests.contains(key) && !drain_requests.contains(key))
            || !times_by_request.emplace(key, record.actual_present_time_ns).second) {
            return false;
        }
    }

    std::optional<uint64_t> previous_present_time_ns;
    for (GameBenchmarkSuccessfulPresentationRequest const& request : evidence.successful_requests) {
        if (request.request.swapchain_generation != *measurement_generation) {
            continue;
        }
        auto const found = times_by_request.find(presentationRequestKey(request.request));
        if (found == times_by_request.end()
            || (previous_present_time_ns && found->second <= *previous_present_time_ns)) {
            return false;
        }
        previous_present_time_ns = found->second;
    }
    return true;
}

void GameBenchmarkTerrainReadinessTracker::observe(
    GameBenchmarkTerrainObservation const& observation,
    std::chrono::steady_clock::time_point const sampled_at
)
{
    ++m_evidence.current_near_sample_count;
    if (!observation.center.has_value()) {
        ++m_evidence.incomplete_current_near_sample_count;
        m_evidence.max_missing_current_mesh_count = std::max(
            m_evidence.max_missing_current_mesh_count,
            GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
        );
        m_evidence.final_center_valid = false;
        m_evidence.final_desired_tile_count = 0U;
        m_evidence.final_resident_tile_count = 0U;
        m_evidence.final_nonresident_tile_count = 0U;
        m_evidence.final_current_mesh_visible_count = 0U;
        m_evidence.final_pending_mesh_count = 0U;
        m_evidence.final_active_mesh_job_count = 0U;
        m_evidence.final_unclassified_missing_count = 0U;
        return;
    }

    shared::HeightTileKey const center = observation.center.value();
    bool const current_near_coverage_complete = observation.desired_count
            == GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
        && observation.current_mesh_visible_count == GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
        && observation.nonresident_count == 0U;
    if (!current_near_coverage_complete) {
        ++m_evidence.incomplete_current_near_sample_count;
        m_evidence.max_missing_current_mesh_count = std::max(
            m_evidence.max_missing_current_mesh_count,
            observation.current_mesh_visible_count < GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
                ? GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT - observation.current_mesh_visible_count
                : 0U
        );
    }
    ++m_evidence.center_sample_count;
    if (m_last_center && m_last_center.value() != center) {
        ++m_evidence.center_change_count;
    }
    m_last_center = center;
    m_evidence.accepted_near_admissions = observation.accepted_near_admissions;
    m_evidence.accepted_far_admissions = observation.accepted_far_admissions;
    m_evidence.far_admissions_while_near_pending = observation.far_admissions_while_near_pending;
    m_evidence.unclassified_missing_observation_count += observation.unclassified_missing_count;
    m_evidence.final_desired_tile_count = observation.desired_count;
    m_evidence.final_resident_tile_count = observation.resident_count;
    m_evidence.final_nonresident_tile_count = observation.nonresident_count;
    m_evidence.final_current_mesh_visible_count = observation.current_mesh_visible_count;
    m_evidence.final_pending_mesh_count = observation.pending_count;
    m_evidence.final_active_mesh_job_count = observation.active_count;
    m_evidence.final_unclassified_missing_count = observation.unclassified_missing_count;
    m_evidence.final_center_valid = true;

    std::set<std::pair<int32_t, int32_t>> desired_keys;
    for (GameBenchmarkTerrainTileObservation const& tile : observation.near_tiles) {
        if (!tile.desired) {
            continue;
        }
        std::pair<int32_t, int32_t> const key = coordinates(tile.key);
        desired_keys.insert(key);
        auto episode = m_open_episodes.find(key);
        if (tile.current_mesh_visible) {
            if (episode != m_open_episodes.end()) {
                uint64_t const latency_ns = static_cast<uint64_t>(std::max<int64_t>(
                    0,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        sampled_at - episode->second.started_at
                    ).count()
                ));
                if (latency_ns > static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE
                        ).count()
                    ) && !episode->second.deadline_missed) {
                    ++m_evidence.deadline_miss_count;
                }
                ++m_evidence.ready_mesh_episode_count;
                if (!m_evidence.has_max_ready_latency || latency_ns > m_evidence.max_ready_latency_ns) {
                    m_evidence.has_max_ready_latency = true;
                    m_evidence.max_ready_latency_ns = latency_ns;
                    m_evidence.max_ready_key_x = tile.key.x;
                    m_evidence.max_ready_key_y = tile.key.y;
                    m_evidence.max_ready_start_center_x = episode->second.start_center.x;
                    m_evidence.max_ready_start_center_y = episode->second.start_center.y;
                }
                m_open_episodes.erase(episode);
            }
            continue;
        }
        if (!tile.resident) {
            if (episode != m_open_episodes.end()) {
                ++m_evidence.source_lost_before_ready_episode_count;
                m_open_episodes.erase(episode);
            }
            continue;
        }
        if (episode == m_open_episodes.end()) {
            m_open_episodes.emplace(key, Episode{ .started_at = sampled_at, .start_center = center });
            ++m_evidence.eligible_mesh_episode_count;
            continue;
        }
        std::chrono::nanoseconds const age = std::chrono::duration_cast<std::chrono::nanoseconds>(
            sampled_at - episode->second.started_at
        );
        if (age > GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE && !episode->second.deadline_missed) {
            episode->second.deadline_missed = true;
            ++m_evidence.deadline_miss_count;
        }
    }

    for (auto episode = m_open_episodes.begin(); episode != m_open_episodes.end();) {
        if (desired_keys.contains(episode->first)) {
            ++episode;
        } else {
            ++m_evidence.left_near_unready_episode_count;
            episode = m_open_episodes.erase(episode);
        }
    }
}

GameBenchmarkTerrainReadinessEvidence GameBenchmarkTerrainReadinessTracker::evidence() const noexcept
{
    GameBenchmarkTerrainReadinessEvidence result = m_evidence;
    result.open_mesh_episode_count = static_cast<uint64_t>(m_open_episodes.size());
    result.current_near_coverage_complete = result.current_near_sample_count != 0U
        && result.incomplete_current_near_sample_count == 0U;
    result.complete = result.center_change_count >= 2U
        && result.eligible_mesh_episode_count > 0U
        && result.ready_mesh_episode_count > 0U
        && result.deadline_miss_count == 0U
        && result.open_mesh_episode_count == 0U
        && result.accepted_near_admissions > 0U
        && result.far_admissions_while_near_pending == 0U
        && result.unclassified_missing_observation_count == 0U
        && result.final_center_valid
        && result.final_desired_tile_count == result.final_current_mesh_visible_count
        && result.final_nonresident_tile_count == 0U
        && result.final_pending_mesh_count == 0U
        && result.final_active_mesh_job_count == 0U
        && result.final_unclassified_missing_count == 0U;
    return result;
}

std::pair<int32_t, int32_t> GameBenchmarkTerrainReadinessTracker::coordinates(
    shared::HeightTileKey const key
) noexcept
{
    return { key.x, key.y };
}

bool gameBenchmarkDiagnosticOverridesPresent(bool const explicit_diagnostic_capture) noexcept
{
    return explicit_diagnostic_capture
        || environmentVariableMatches("MC_DIAGNOSTIC_DIRECTIONAL_STONE_CULLING", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_DIRECT_STONE_DRAWS", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_GPU_TIMESTAMPS", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_INDEXED_STONE_QUADS", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_PRESENTATION_STAGES", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_TERRAIN_SPATIAL_SCAN", std::nullopt)
        || environmentVariableMatches("MC_DIAGNOSTIC_TERRAIN_OCCLUSION", std::nullopt);
}

GameBenchmarkPhase gameBenchmarkPhaseAt(
    Clock::time_point const completed_at,
    Clock::time_point const first_loop_at,
    GameBenchmarkOptions const& options
) noexcept
{
    if (completed_at < first_loop_at) {
        return GameBenchmarkPhase::Outside;
    }
    if (completed_at < first_loop_at + options.cold_duration) {
        return GameBenchmarkPhase::Cold;
    }
    if (completed_at < first_loop_at + options.cold_duration + options.warm_duration) {
        return GameBenchmarkPhase::Warm;
    }
    if (completed_at < first_loop_at + options.cold_duration + options.warm_duration
        + options.uncapped_duration) {
        return GameBenchmarkPhase::Uncapped;
    }
    return GameBenchmarkPhase::Outside;
}

std::expected<RuntimeEvidence, std::string> runGameBenchmark(GameBenchmarkOptions const& options)
{
    bool const s1_terrain_workload = options.workload == GameBenchmarkWorkload::OrdinaryTurningMovement;
    bool const full_radius_workload = options.workload == GameBenchmarkWorkload::Speed200FullRadiusRecovery;
    bool capture_presentation_history = options.qualification_proof == "performance";
#if !defined(__APPLE__)
    capture_presentation_history = false;
#endif
    if (!options.qualification_proof.empty()) {
        bool const proof_workload_matches = options.qualification_proof == "performance"
            ? options.workload == GameBenchmarkWorkload::OrdinaryTurningMovement
            : options.workload == GameBenchmarkWorkload::Speed200FullRadiusRecovery;
        uint32_t const required_radius = 256U;
        bool const legacy_timeout_mismatch = options.qualification_proof == "terrain-coverage"
            && options.full_radius_warmup_timeout != GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT;
        if ((options.qualification_proof != "terrain-coverage" && options.qualification_proof != "performance")
            || !proof_workload_matches || options.render_distance != required_radius
            || legacy_timeout_mismatch
            || gameBenchmarkDiagnosticOverridesPresent(options.diagnostic_full_radius_capture)) {
            return std::unexpected("gameplay proof requires its fixed radius and production settings");
        }
    }
    std::chrono::seconds const effective_deadline = gameBenchmarkDeadline(options);
    if (options.cold_duration <= std::chrono::seconds::zero()
        || options.warm_duration <= std::chrono::seconds::zero()
        || options.uncapped_duration <= std::chrono::seconds::zero()
        || (!full_radius_workload && options.deadline <= options.cold_duration + options.warm_duration + options.uncapped_duration
            + (options.workload == GameBenchmarkWorkload::Speed200Movement
                ? GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE : std::chrono::milliseconds::zero()))) {
        return std::unexpected("game benchmark has invalid duration limits");
    }
    if (full_radius_workload
        && !gameBenchmarkFullRadiusWarmupTimeout(options.full_radius_warmup_timeout.count())) {
        return std::unexpected("full-radius warmup timeout must be from 1 to 1800 seconds");
    }
    if (gameBenchmarkWorkloadName(options.workload) == "unknown") {
        return std::unexpected("game benchmark has an unknown workload");
    }
    if (!shared::isValidHeightTileInterestRadius(options.render_distance)) {
        return std::unexpected("render distance must be an integer chunk radius from 1 to 256");
    }
    shared::WorldConfiguration const world_configuration = shared::World::canonicalConfiguration();
    Clock::time_point const started_at = Clock::now();
    std::cout << "[benchmark-game] preparing canonical Terrain-v2 world\n" << std::flush;
    auto const world_preparation = startBenchmarkTerrainPreparation(
        world_configuration,
        started_at,
        options.backend
    );
    if (!s1_terrain_workload) {
        awaitBenchmarkTerrainPreparation(world_preparation);
    }
    server::WorldPreparationSnapshot const preparation_snapshot = world_preparation->latest();
    std::optional<shared::corelang::BackendReceipt> terrain_backend_receipt;
    if (preparation_snapshot.terrain_plan) {
        terrain_backend_receipt = preparation_snapshot.terrain_plan->receipt;
    }
    if (!s1_terrain_workload && !terrain_backend_receipt) {
        return std::unexpected("Terrain-v2 benchmark preparation omitted its CoreLang backend receipt");
    }
    if (!s1_terrain_workload && terrain_backend_receipt
        && (terrain_backend_receipt->requested_backend != options.backend
            || terrain_backend_receipt->actual_backend != options.backend)) {
        return std::unexpected("Terrain-v2 benchmark preparation reported a mismatched CoreLang backend");
    }
    std::shared_ptr<shared::TerrainWorld const> const terrain_world = s1_terrain_workload
        ? nullptr : world_preparation->readyWorld();
    std::vector<server::GameServer::SpawnPoint> spawn_points;
    if (options.workload == GameBenchmarkWorkload::WrappedBorder) {
        spawn_points.push_back({
            .character = '@',
            .x = shared::World::FLIGHT_MAX_CELL - 2,
            .y = shared::World::FLIGHT_SPAWN.y,
            .z = shared::World::FLIGHT_SPAWN.z,
        });
    } else if (!s1_terrain_workload) {
        static constexpr int32_t SPAWN_X = 32'768;
        static constexpr int32_t SPAWN_Y = 32'768;
        uint16_t const spawn_surface_height = terrain_world->sampleSurfaceHeight(SPAWN_X, SPAWN_Y);
        spawn_points.push_back({
            .character = '@',
            .x = SPAWN_X,
            .y = SPAWN_Y,
            .z = static_cast<int32_t>(spawn_surface_height) + 1,
        });
    }
    server::GameServer server{
        0U, spawn_points, shared::WorldMode::Flight,
        world_configuration, options.render_distance, terrain_world, world_preparation,
    };
    std::vector<ServerTickSample> raw_server_ticks;
    raw_server_ticks.reserve(512U);
    std::vector<GameBenchmarkServerStreamSample> raw_server_stream_samples;
    if (options.diagnostic_full_radius_capture || s1_terrain_workload) {
        raw_server_stream_samples.reserve(16'000U);
    }
    std::vector<std::chrono::nanoseconds> raw_permission_publish_durations;
    std::optional<std::string> permission_error;
    std::atomic<int64_t> measurement_end_ns{ 0 };
    std::atomic<int64_t> first_loop_started_at_ns{ 0 };
    uint64_t measured_tick_count = 0U;
    uint64_t permission_publish_count = 0U;
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = [&](std::chrono::nanoseconds const duration, uint64_t const events) {
            raw_server_ticks.push_back(ServerTickSample{
                .completed_at = Clock::now(),
                .duration = duration,
                .network_events = events,
            });
            int64_t const end_ns = measurement_end_ns.load(std::memory_order_acquire);
            if (options.workload != GameBenchmarkWorkload::PermissionCollisionChurn
                || end_ns == 0 || Clock::now().time_since_epoch() >= std::chrono::nanoseconds{ end_ns }
                || permission_error) {
                return;
            }
            ++measured_tick_count;
            if (measured_tick_count % 10U != 0U) {
                return;
            }
            auto const publish_started_at = Clock::now();
            bool const allow_collision_bypass = permission_publish_count % 2U != 0U;
            auto compilation = compileMovementPolicy(allow_collision_bypass);
            if (!compilation) {
                permission_error = compilation.error().message;
                return;
            }
            auto published = server.publishPermissions(std::move(*compilation), movementPolicyRegistry());
            if (!published) {
                permission_error = published.error().message;
                return;
            }
            raw_permission_publish_durations.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - publish_started_at)
            );
            ++permission_publish_count;
        },
        .on_preview_buffered = {},
        .on_preview_metrics = options.diagnostic_full_radius_capture || s1_terrain_workload
            ? std::function<void(core::ClientId, server::GameServer::PreviewStreamMetrics const&)>{
                [&](core::ClientId, server::GameServer::PreviewStreamMetrics const& metrics) {
                    int64_t const first_loop_ns = first_loop_started_at_ns.load(std::memory_order_acquire);
                    if (first_loop_ns == 0 || !metrics.terrain_center_valid) {
                        return;
                    }
                    int64_t const sample_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()
                    ).count();
                    if (sample_ns < first_loop_ns) {
                        return;
                    }
                    raw_server_stream_samples.push_back({
                        .started_offset_ns = static_cast<uint64_t>(sample_ns - first_loop_ns),
                        .center_x = metrics.terrain_center_x,
                        .center_y = metrics.terrain_center_y,
                        .near_desired_count = metrics.near_window_desired_tiles,
                        .near_resident_count = metrics.near_window_resident_tiles,
                        .near_queued_count = metrics.near_window_queued_tiles,
                        .near_dispatched_count = metrics.near_window_dispatched_tiles,
                        .near_ready_count = metrics.near_window_ready_tiles,
                        .near_inflight_count = metrics.near_window_inflight_additions,
                        .near_inflight_removals_count = metrics.near_window_inflight_removals,
                        .near_untracked_count = metrics.near_window_untracked_tiles,
                        .server_queued_count = metrics.queued_tiles,
                        .server_dispatched_count = metrics.dispatched_tiles,
                        .server_ready_count = metrics.ready_tiles,
                        .server_inflight_addition_count = metrics.inflight_additions,
                        .server_inflight_removal_count = metrics.inflight_removals,
                        .server_inflight_delivery_count = metrics.inflight_deliveries,
                        .server_pending_removal_count = metrics.pending_removals,
                        .server_resident_count = metrics.resident_tiles,
                        .server_admitted_addition_count = metrics.admitted_additions,
                        .server_rejected_delivery_ack_count = metrics.rejected_delivery_acknowledgements,
                        .server_delivery_credit_count = metrics.delivery_credits,
                        .worker_queued_count = metrics.height_tile_jobs.executor_queued_jobs,
                        .worker_running_count = metrics.height_tile_jobs.running_jobs,
                        .worker_completed_uncollected_count =
                            metrics.height_tile_jobs.completed_uncollected_jobs,
                        .world_generation_worker_queued_count =
                            metrics.world_generation_jobs.executor_queued_jobs,
                        .world_generation_worker_running_count = metrics.world_generation_jobs.running_jobs,
                        .world_generation_worker_completed_uncollected_count =
                            metrics.world_generation_jobs.completed_uncollected_jobs,
                        .coverage_refresh = {
                            .calls = metrics.coverage_refresh.calls,
                            .duration_ns = positiveNanoseconds(metrics.coverage_refresh.duration),
                        },
                        .worker_result_publication = {
                            .calls = metrics.worker_result_publication.calls,
                            .duration_ns = positiveNanoseconds(metrics.worker_result_publication.duration),
                        },
                        .queue_refill = {
                            .calls = metrics.queue_refill.calls,
                            .duration_ns = positiveNanoseconds(metrics.queue_refill.duration),
                        },
                        .delivery = {
                            .calls = metrics.delivery.calls,
                            .duration_ns = positiveNanoseconds(metrics.delivery.duration),
                        },
                        .worker_dispatch = {
                            .calls = metrics.worker_dispatch.calls,
                            .duration_ns = positiveNanoseconds(metrics.worker_dispatch.duration),
                        },
                        .materialization_candidate_keys_examined =
                            metrics.materialization_candidate_keys_examined,
                        .materialization_resident_chunk_lookups =
                            metrics.materialization_resident_chunk_lookups,
                        .materialization_admission_attempts = metrics.materialization_admission_attempts,
                        .materialization_admissions = metrics.materialization_admissions,
                        .materialization_duplicate_requests = metrics.materialization_duplicate_requests,
                        .chunk_delivery_candidate_keys_examined = metrics.chunk_delivery_candidate_keys_examined,
                        .chunk_delivery_resident_chunk_lookups = metrics.chunk_delivery_resident_chunk_lookups,
                        .chunk_delivery_batches_sent = metrics.chunk_delivery_batches_sent,
                        .chunk_delivery_chunks_sent = metrics.chunk_delivery_chunks_sent,
                        .chunk_delivery_credits_consumed = metrics.chunk_delivery_credits_consumed,
                        .world_generation_jobs_dispatched = metrics.world_generation_jobs_dispatched,
                        .height_tile_jobs_dispatched = metrics.height_tile_jobs_dispatched,
                        .global_server_world_resident_chunks = metrics.server_world_resident_chunks,
                        .required_near_chunk_keys = metrics.required_near_chunk_keys,
                        .required_full_chunk_keys = metrics.required_full_chunk_keys,
                        .acknowledged_chunk_keys = metrics.acknowledged_chunk_keys,
                        .inflight_chunk_batches = metrics.inflight_chunk_batches,
                        .inflight_chunk_keys = metrics.inflight_chunk_keys,
                        .chunk_delivery_credits = metrics.chunk_delivery_credits,
                        .near_required_chunks_all_resident = metrics.near_required_chunks_all_resident,
                    });
                },
            }
            : std::function<void(core::ClientId, server::GameServer::PreviewStreamMetrics const&)>{},
        .before_height_tile_completion = {},
    };
    ServerRunGuard server_run{ server, server_hooks };
    std::cout << "[benchmark-game] loopback server started\n" << std::flush;
    GameBenchmarkFullRadiusSequence full_radius_sequence{ options.full_radius_warmup_timeout };
    std::optional<Clock::time_point> full_radius_movement_started_at;
    std::optional<Clock::time_point> movement_route_started_at;
    GameBenchmarkS1MovementWindow s1_movement_window;
    std::optional<GameBenchmarkMovementRouteSample> pending_movement_route;
    std::unordered_set<uint32_t> movement_input_sequences;
    movement_input_sequences.reserve(512U);
    std::unordered_set<uint32_t> s1_movement_input_sequences;
    s1_movement_input_sequences.reserve(512U);
    std::unordered_set<uint32_t> s1_pre_window_input_sequences;
    s1_pre_window_input_sequences.reserve(512U);
    std::unordered_set<uint32_t> s1_post_window_input_sequences;
    s1_post_window_input_sequences.reserve(32U);
    bool s1_movement_window_drained = false;
    auto const player_storage = std::make_unique<client::PlayerClient>(
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{
            .require_immediate_present_mode = options.require_immediate_present_mode,
            .freeze_camera = options.workload == GameBenchmarkWorkload::DiagnosticStationary
                || full_radius_workload,
            .show_debug_hud = full_radius_workload || s1_terrain_workload,
            .enable_mesh_stage_diagnostics = options.diagnostic_full_radius_capture,
            .enable_presentation_history = capture_presentation_history,
            .capture_frame_completion = true,
            .camera_angles_override = options.workload == GameBenchmarkWorkload::OrdinaryTurningMovement
                ? std::function<std::optional<client::CameraAngles>(Clock::time_point)>{
                    [&movement_route_started_at](Clock::time_point const sampled_at)
                        -> std::optional<client::CameraAngles> {
                        if (!movement_route_started_at) {
                            return std::nullopt;
                        }
                        std::chrono::nanoseconds const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            sampled_at - *movement_route_started_at
                        );
                        GameBenchmarkMovementRouteSample const route = gameBenchmarkRepeatingMovementRouteAt(elapsed);
                        return client::CameraAngles{
                            .yaw_degrees = route.yaw_degrees,
                            .pitch_degrees = route.pitch_degrees,
                        };
                    },
                }
                : std::function<std::optional<client::CameraAngles>(Clock::time_point)>{},
        }
    );
    client::PlayerClient& player = *player_storage;
    std::cout << "[benchmark-game] player renderer ready; connecting\n" << std::flush;

    // Direct construction avoids a full-size aggregate temporary on this stack.
    std::unique_ptr<GameBenchmarkEvidence> benchmark_storage{ new GameBenchmarkEvidence{
        .workload = std::string{ gameBenchmarkWorkloadName(options.workload) },
        .qualification_proof = options.qualification_proof,
        .movement_trajectory_contract = options.workload == GameBenchmarkWorkload::OrdinaryTurningMovement
            ? "mc.x30-authoritative-trajectory.v1" : "",
        .terrain_backend_receipt = terrain_backend_receipt,
        .package_id = std::string{ shared::PROJECT_NAME },
        .hardware = hardwareName(),
        .gpu = std::nullopt,
        .present_mode = {},
#if defined(NDEBUG)
        .build_mode = "release",
#else
        .build_mode = "debug",
#endif
        .qualification_settings_manifest = s1_terrain_workload
            ? std::string{GAME_BENCHMARK_S1_QUALIFICATION_MANIFEST} : std::string{},
        .terrain_identity = std::nullopt,
        .diagnostic_overrides_present = gameBenchmarkDiagnosticOverridesPresent(
            options.diagnostic_full_radius_capture
        ),
        .cold_duration_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(options.cold_duration).count()
        ),
        .warm_duration_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(options.warm_duration).count()
        ),
        .uncapped_duration_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(options.uncapped_duration).count()
        ),
        .cold_loop_timings = {},
        .warm_loop_timings = {},
        .uncapped_loop_timings = {},
        .server_tick_timings = {},
        .permission_publish_timings = {},
        .frame_budget = std::nullopt,
        .terrain_readiness = std::nullopt,
        .full_radius_terrain = full_radius_workload
            ? std::optional<GameBenchmarkFullRadiusEvidence>{GameBenchmarkFullRadiusEvidence{
                .render_distance = options.render_distance,
                .expected_tile_count = shared::heightTileInterestCount(options.render_distance),
                .warmup_timeout_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        options.full_radius_warmup_timeout
                    ).count()
                ),
                .initial_barrier = {},
                .recovery_barrier = {},
                .baseline_loop_timings = {},
                .recovered_loop_timings = {},
                .baseline_successful_present_request_intervals = {},
                .recovered_successful_present_request_intervals = {},
                .movement_playable_sphere_sample_scope =
                    "per-movement-loop-and-every-authoritative-position-update",
                .movement_authoritative_sphere_updates = {},
            }}
            : std::nullopt,
        .s1_terrain_performance = s1_terrain_workload
            ? std::optional<GameBenchmarkS1TerrainPerformanceEvidence>{GameBenchmarkS1TerrainPerformanceEvidence{}}
            : std::nullopt,
        .presentation_timing = capture_presentation_history
            ? std::optional<GameBenchmarkPresentationTimingEvidence>{GameBenchmarkPresentationTimingEvidence{}}
            : std::nullopt,
        .controlled_start = GameBenchmarkStartActionEvidence{},
        .raw_gameplay_inputs = {},
        .raw_frames = {},
        .raw_fence_confirmed_frames = {},
        .raw_server_stream_samples = {},
        .raw_server_tick_durations_ns = {},
        .raw_permission_publish_durations_ns = {},
    }};
    GameBenchmarkEvidence& benchmark = *benchmark_storage;
    benchmark.raw_frames.reserve(20'000U);
    benchmark.raw_gameplay_inputs.reserve(2'000U);
    if (benchmark.presentation_timing) {
        benchmark.presentation_timing->successful_requests.reserve(50'000U);
        benchmark.presentation_timing->records.reserve(50'000U);
        benchmark.presentation_timing->polls.reserve(50'000U);
        benchmark.presentation_timing->drain_request_ids.reserve(120U);
    }
    std::vector<std::chrono::nanoseconds> cold_durations;
    std::vector<std::chrono::nanoseconds> warm_durations;
    std::vector<std::chrono::nanoseconds> uncapped_durations;
    uint64_t loop_ordinal = 0U;
    std::vector<std::chrono::nanoseconds> baseline_full_radius_durations;
    std::vector<std::chrono::nanoseconds> recovered_full_radius_durations;
    GameBenchmarkTerrainReadinessTracker terrain_readiness_tracker;
    uint64_t last_accepted_near_admissions = 0U;
    uint64_t last_accepted_far_admissions = 0U;
    uint64_t last_far_admissions_while_near_pending = 0U;
    std::optional<Clock::time_point> first_loop_at;
    Clock::time_point last_loading_progress_at = started_at;
    std::optional<Clock::time_point> last_full_progress_at;
    std::optional<Clock::time_point> last_full_warmup_raw_at;
    bool full_radius_movement_window_complete = false;
    std::optional<Clock::time_point> last_full_spatial_scan_at;
    std::optional<GameBenchmarkFullRadiusPhase> last_full_spatial_scan_phase;
    std::optional<shared::HeightTileKey> last_movement_center;
    bool full_initial_candidate_scanned = false;
    bool full_recovery_candidate_scanned = false;
    bool const full_radius_spatial_scan_enabled = options.diagnostic_full_radius_capture
        || environmentVariableMatches("MC_DIAGNOSTIC_TERRAIN_SPATIAL_SCAN", "1");
    uint8_t reported_phase = 0U;
    uint64_t warm_successful_present_requests = 0U;
    uint64_t uncapped_successful_present_requests = 0U;
    std::optional<shared::Player> previous_authoritative_player;
    uint64_t authoritative_state_ordinal = 0U;
    std::optional<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
        latest_s1_authoritative_state;
    std::optional<Clock::time_point> full_radius_measurement_end_at;
    std::optional<Clock::time_point> presentation_drain_started_at;
    uint32_t presentation_drain_attempts = 0U;
    bool presentation_capture_identity_complete = true;
    bool presentation_capture_poll_failed = false;
    bool presentation_capture_generation_changed = false;
    bool presentation_capture_capability_observed = false;
    bool presentation_capture_capability_supported = false;
    bool presentation_capture_generation_observed = false;
    bool presentation_capture_generation_locked = false;
    uint64_t presentation_capture_generation = 0U;
    uint64_t unresolved_expected_presentations = 0U;
    std::unordered_set<PresentationRequestKey, PresentationRequestKeyHash> expected_presentation_requests;
    std::unordered_set<PresentationRequestKey, PresentationRequestKeyHash> recorded_presentation_requests;
    std::unordered_set<PresentationRequestKey, PresentationRequestKeyHash> drain_presentation_requests;
    std::unordered_map<uint64_t, uint32_t> last_present_id_by_generation;
    GameBenchmarkFrameCompletionTracker frame_completion_tracker;
    size_t retired_frame_completions_consumed = 0U;
    uint64_t next_frame_sequence_id = 1U;
    expected_presentation_requests.reserve(50'000U);
    recorded_presentation_requests.reserve(50'120U);
    drain_presentation_requests.reserve(120U);
    last_present_id_by_generation.reserve(16U);
    auto const offsetFromBenchmarkStart = [&](Clock::time_point const sampled_at) {
        return benchmarkOffsetFromStart(first_loop_at, sampled_at);
    };
    auto const offsetFromRequestAccepted = [&](Clock::time_point const sampled_at) {
        return sampled_at >= started_at
            ? std::optional<uint64_t>{ positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
                sampled_at - started_at
            )) }
            : std::nullopt;
    };
    auto const observeControlledStart = [&](client::WorldLoadingStatus const& status) {
        GameBenchmarkStartActionEvidence& evidence = *benchmark.controlled_start;
        if (status.ready_prompt_frame_rendered_at && !evidence.eligible_prompt_rendered_offset_ns) {
            evidence.eligible_prompt_rendered_offset_ns =
                offsetFromRequestAccepted(*status.ready_prompt_frame_rendered_at);
        }
        if (status.key_accepted_at && !evidence.key_accepted_offset_ns) {
            evidence.key_accepted_offset_ns = offsetFromRequestAccepted(*status.key_accepted_at);
        }
        if (status.start_acknowledged_at && !evidence.server_start_acknowledged_offset_ns) {
            evidence.server_start_acknowledged_offset_ns =
                offsetFromRequestAccepted(*status.start_acknowledged_at);
        }
        evidence.complete = evidence.synthetic_input_override_invoked
            && evidence.eligible_prompt_rendered_offset_ns
            && evidence.synthetic_input_requested_offset_ns
            && evidence.key_accepted_offset_ns
            && evidence.server_start_acknowledged_offset_ns
            && evidence.input_ordinal
            && *evidence.eligible_prompt_rendered_offset_ns
                <= *evidence.synthetic_input_requested_offset_ns
            && *evidence.synthetic_input_requested_offset_ns <= *evidence.key_accepted_offset_ns
            && *evidence.key_accepted_offset_ns <= *evidence.server_start_acknowledged_offset_ns;
    };
    auto const observeS1LoadingStatus = [&](
        client::WorldLoadingStatus const& status,
        Clock::time_point const sampled_at
    ) {
        if (!benchmark.s1_terrain_performance) {
            return;
        }
        GameBenchmarkS1TerrainPerformanceEvidence& evidence = *benchmark.s1_terrain_performance;
        if (status.ready_prompt_frame_rendered_at && !evidence.ready_prompt_frame_offset_ns) {
            evidence.ready_prompt_frame_offset_ns = offsetFromRequestAccepted(
                *status.ready_prompt_frame_rendered_at
            );
        }
        if (status.playable_sphere_first_ready_at && !evidence.playable_sphere_first_ready_offset_ns) {
            evidence.playable_sphere_first_ready_offset_ns = offsetFromRequestAccepted(
                *status.playable_sphere_first_ready_at
            );
        }
        evidence.initial_deadline_missed = status.ready_prompt_deadline_missed
            || (status.ready_prompt_frame_rendered_at
                && *status.ready_prompt_frame_rendered_at > status.ready_prompt_deadline)
            || (!status.ready_prompt_frame_rendered_at && sampled_at >= status.ready_prompt_deadline);
        evidence.playable_deadline_missed = status.full_radius_deadline_missed
            || (status.playable_sphere_first_ready_at
                && *status.playable_sphere_first_ready_at > status.full_radius_deadline)
            || (!status.playable_sphere_first_ready_at && sampled_at >= status.full_radius_deadline);
    };
    auto const captureFenceConfirmedFrame = [&](
        client::RendererRuntimeInfo const& runtime,
        Clock::time_point const observed_at
    ) {
        core::graphics::vulkan::PresentationAcquireDiagnostics const& diagnostics =
            runtime.diagnostic_presentation_acquire;
        if (!diagnostics.captured || diagnostics.frame_slot >= GameBenchmarkFrameCompletionTracker::FRAME_SLOT_COUNT) {
            return;
        }
        bool const submitted_frame_fence_succeeded = std::ranges::any_of(
            std::span{ diagnostics.stages.data(), diagnostics.stage_count },
            [](core::graphics::vulkan::PresentationAcquireStageTiming const& stage) {
                return stage.stage == core::graphics::vulkan::PresentationAcquireStage::SubmittedFrameFence
                    && stage.result == VK_SUCCESS;
            }
        );
        if (!submitted_frame_fence_succeeded) {
            return;
        }
        std::optional<uint64_t> const observed_offset = offsetFromBenchmarkStart(observed_at);
        if (!observed_offset) {
            return;
        }
        std::optional<GameBenchmarkFrameCompletion> completion = frame_completion_tracker.confirmFence(
            runtime.current_swapchain_generation,
            diagnostics.frame_slot,
            *observed_offset
        );
        if (completion) {
            benchmark.raw_fence_confirmed_frames.push_back(std::move(completion).value());
        }
    };
    auto const recordFrameSubmission = [&](
        client::GameClientLoopSample const& sample,
        client::RendererRuntimeInfo const& runtime,
        std::string_view const phase
    ) -> std::optional<uint64_t> {
        if (!sample.presentation_succeeded) {
            return std::nullopt;
        }
        uint64_t const sequence_id = next_frame_sequence_id++;
        std::optional<uint64_t> const submitted_offset = offsetFromBenchmarkStart(sample.completed_at);
        core::graphics::vulkan::PresentationAcquireDiagnostics const& diagnostics =
            runtime.diagnostic_presentation_acquire;
        if (!submitted_offset || !diagnostics.captured
            || diagnostics.frame_slot >= GameBenchmarkFrameCompletionTracker::FRAME_SLOT_COUNT) {
            return sequence_id;
        }
        static_cast<void>(frame_completion_tracker.track(GameBenchmarkPendingFrameSubmission{
            .sequence_id = sequence_id,
            .swapchain_generation = runtime.successful_presentation_swapchain_generation,
            .frame_slot = diagnostics.frame_slot,
            .submitted_offset_ns = *submitted_offset,
            .application_present_id = runtime.successful_presentation_id,
            .submission_phase = std::string{ phase },
        }));
        return sequence_id;
    };
    auto const observePresentationGeneration = [&](
        client::RendererRuntimeInfo const& runtime,
        bool const measurement_phase_started
    ) {
        GameBenchmarkPresentationTimingEvidence& evidence = *benchmark.presentation_timing;
        if (!presentation_capture_capability_observed) {
            presentation_capture_capability_observed = true;
            presentation_capture_capability_supported = runtime.presentation_history_enabled;
            evidence.capability_supported = runtime.presentation_history_enabled;
        } else if (runtime.presentation_history_enabled != presentation_capture_capability_supported) {
            presentation_capture_poll_failed = true;
        }

        if (!presentation_capture_generation_observed) {
            presentation_capture_generation_observed = true;
            presentation_capture_generation = runtime.current_swapchain_generation;
        } else if (presentation_capture_generation_locked) {
            if (runtime.current_swapchain_generation != presentation_capture_generation) {
                presentation_capture_generation_changed = true;
            }
        } else {
            if (runtime.current_swapchain_generation < presentation_capture_generation
                || (measurement_phase_started
                    && runtime.current_swapchain_generation != presentation_capture_generation)) {
                presentation_capture_generation_changed = true;
            }
            if (runtime.current_swapchain_generation >= presentation_capture_generation) {
                presentation_capture_generation = runtime.current_swapchain_generation;
            }
        }

        if (measurement_phase_started && !presentation_capture_generation_locked) {
            presentation_capture_generation = runtime.current_swapchain_generation;
            presentation_capture_generation_locked = true;
            unresolved_expected_presentations = 0U;
            for (GameBenchmarkSuccessfulPresentationRequest const& request : evidence.successful_requests) {
                PresentationRequestKey const key = presentationRequestKey(request.request);
                if (request.request.swapchain_generation == presentation_capture_generation
                    && !recorded_presentation_requests.contains(key)) {
                    ++unresolved_expected_presentations;
                }
            }
        }
    };
    auto const captureSuccessfulRequest = [&](
        client::GameClientLoopSample const& sample,
        client::RendererRuntimeInfo const& runtime,
        std::string_view const phase,
        uint64_t const ordinal,
        bool const draining
    ) -> std::optional<GameBenchmarkPresentationRequestId> {
        if (!sample.presentation_succeeded) {
            return std::nullopt;
        }
        if (!runtime.successful_presentation_id.has_value()
            || (!presentation_capture_generation_locked
                && phase != "initial-full-radius-warmup"
                && runtime.successful_presentation_swapchain_generation != runtime.current_swapchain_generation)) {
            presentation_capture_identity_complete = false;
            return std::nullopt;
        }
        GameBenchmarkPresentationRequestId const request{
            .swapchain_generation = runtime.successful_presentation_swapchain_generation,
            .application_present_id = *runtime.successful_presentation_id,
        };
        if (request.application_present_id == 0U) {
            presentation_capture_identity_complete = false;
        }
        if (request.swapchain_generation > runtime.current_swapchain_generation
            || (phase != "initial-full-radius-warmup"
                && request.swapchain_generation != presentation_capture_generation)) {
            presentation_capture_generation_changed = true;
        }
        auto const last_id = last_present_id_by_generation.try_emplace(
            request.swapchain_generation,
            0U
        ).first;
        if (request.application_present_id <= last_id->second) {
            presentation_capture_identity_complete = false;
        } else {
            last_id->second = request.application_present_id;
        }
        PresentationRequestKey const key = presentationRequestKey(request);
        if (draining) {
            if (!drain_presentation_requests.insert(key).second) {
                presentation_capture_identity_complete = false;
            }
            benchmark.presentation_timing->drain_request_ids.push_back(request);
        } else {
            bool const inserted_request = expected_presentation_requests.insert(key).second;
            if (!inserted_request) {
                presentation_capture_identity_complete = false;
            }
            benchmark.presentation_timing->successful_requests.push_back({
                .request = request,
                .loop_ordinal = ordinal,
                .phase = std::string{ phase },
            });
            if (inserted_request && presentation_capture_generation_locked
                && request.swapchain_generation == presentation_capture_generation) {
                ++unresolved_expected_presentations;
            }
        }
        return request;
    };
    auto const pollPresentationHistory = [&](GameBenchmarkPresentationPollScope const scope) {
        std::array<core::graphics::vulkan::PresentationTiming, 256U> records{};
        core::graphics::vulkan::PresentationHistoryResult const query =
            player.benchmarkQueryPresentationHistory(records);
        GameBenchmarkPresentationPollStatus status = GameBenchmarkPresentationPollStatus::Error;
        switch (query.status) {
        case core::graphics::vulkan::PresentationHistoryStatus::Unavailable:
            status = GameBenchmarkPresentationPollStatus::Unavailable;
            break;
        case core::graphics::vulkan::PresentationHistoryStatus::Empty:
            status = GameBenchmarkPresentationPollStatus::Empty;
            break;
        case core::graphics::vulkan::PresentationHistoryStatus::Records:
            status = GameBenchmarkPresentationPollStatus::Records;
            break;
        case core::graphics::vulkan::PresentationHistoryStatus::Incomplete:
            status = GameBenchmarkPresentationPollStatus::Incomplete;
            break;
        case core::graphics::vulkan::PresentationHistoryStatus::Error:
            status = GameBenchmarkPresentationPollStatus::Error;
            break;
        }
        uint32_t const safe_record_count = std::min(
            query.record_count,
            static_cast<uint32_t>(records.size())
        );
        if (query.record_count > records.size()) {
            presentation_capture_poll_failed = true;
        }
        benchmark.presentation_timing->polls.push_back({
            .scope = scope,
            .status = status,
            .vk_result = static_cast<int32_t>(query.result),
            .returned_count = safe_record_count,
        });
        if (status == GameBenchmarkPresentationPollStatus::Unavailable
            || status == GameBenchmarkPresentationPollStatus::Error) {
            presentation_capture_poll_failed = true;
        }
        for (uint32_t index = 0U; index < safe_record_count; ++index) {
            core::graphics::vulkan::PresentationTiming const& source = records[index];
            GameBenchmarkPresentationRequestId const request{
                .swapchain_generation = source.swapchain_generation,
                .application_present_id = source.present_id,
            };
            uint64_t const actual_present_time_ns = source.actual_present_time.count();
            benchmark.presentation_timing->records.push_back({
                .request = request,
                .actual_present_time_ns = actual_present_time_ns,
            });
            PresentationRequestKey const key = presentationRequestKey(request);
            bool const expected_request = expected_presentation_requests.contains(key);
            if (request.application_present_id == 0U
                || (!expected_request && !drain_presentation_requests.contains(key))) {
                presentation_capture_poll_failed = true;
                continue;
            }
            if (!recorded_presentation_requests.insert(key).second) {
                presentation_capture_poll_failed = true;
                continue;
            }
            if (expected_request && presentation_capture_generation_locked
                && request.swapchain_generation == presentation_capture_generation
                && unresolved_expected_presentations != 0U) {
                --unresolved_expected_presentations;
            }
        }
    };
    auto const updatePresentationDrainCompletion = [&] {
        if (!benchmark.presentation_timing || !benchmark.presentation_timing->capture_endpoint) {
            return;
        }
        benchmark.presentation_timing->drain_complete = presentation_capture_capability_supported
            && presentation_capture_identity_complete
            && !presentation_capture_poll_failed
            && !presentation_capture_generation_changed
            && unresolved_expected_presentations == 0U;
        if (benchmark.presentation_timing->drain_complete) {
            benchmark.presentation_timing->drain_complete = gameBenchmarkPresentationTimingComplete(
                *benchmark.presentation_timing
            );
        }
    };
    auto const captureS1PresentationEndpoint = [&](Clock::time_point const sampled_at) {
        if (!s1_terrain_workload || !benchmark.presentation_timing
            || benchmark.presentation_timing->capture_endpoint) {
            return;
        }
        std::optional<Clock::time_point> const movement_started_at = s1_movement_window.startedAt();
        if (!movement_started_at
            || sampled_at < *movement_started_at + GAME_BENCHMARK_S1_MOVEMENT_WINDOW) {
            return;
        }
        std::vector<GameBenchmarkSuccessfulPresentationRequest> const& requests =
            benchmark.presentation_timing->successful_requests;
        if (requests.empty() || requests.back().phase != S1_MOVEMENT_FRAME_PHASE) {
            return;
        }
        GameBenchmarkSuccessfulPresentationRequest const& endpoint_request = requests.back();
        benchmark.presentation_timing->capture_endpoint = GameBenchmarkPresentationCaptureEndpoint{
            .request = endpoint_request.request,
            .loop_ordinal = endpoint_request.loop_ordinal,
        };
        presentation_drain_started_at = *movement_started_at + GAME_BENCHMARK_S1_MOVEMENT_WINDOW;
        updatePresentationDrainCompletion();
    };
    struct LoopScratch final {
        ReusableHeapValue<client::RendererRuntimeInfo> runtime;
        ReusableHeapValue<client::WorldLoadingStatus> loading_status;
        ReusableHeapValue<client::PlayerClientRenderDiagnostics> render_diagnostics;
        ReusableHeapValue<client::PlayerClientBenchmarkFullTerrainInfo> full_terrain_info;
        ReusableHeapValue<std::optional<GameBenchmarkFullTerrainSnapshot>> full_radius_snapshot;
        ReusableHeapValue<client::PlayerClientBenchmarkTerrainInfo> terrain_info;
        ReusableHeapValue<GameBenchmarkFrameSample> frame_sample;
    };
    auto loop_scratch = std::make_unique<LoopScratch>();
    client::GameClientBenchmarkHooks const client_hooks{
        .deadline = started_at + effective_deadline,
        .should_stop = [&] {
            if (s1_terrain_workload && !s1_movement_window.startedAt()) {
                Clock::time_point const now = Clock::now();
                client::WorldLoadingStatus const status = player.worldLoadingStatus();
                observeS1LoadingStatus(status, now);
                if (status.full_radius_deadline != Clock::time_point{}
                    && now >= status.full_radius_deadline) {
                    return true;
                }
            }
            if (!first_loop_at) {
                return false;
            }
            if (s1_terrain_workload) {
                std::optional<Clock::time_point> const movement_started_at = s1_movement_window.startedAt();
                if (!movement_started_at) {
                    return false;
                }
                Clock::time_point const movement_end = *movement_started_at
                    + GAME_BENCHMARK_S1_MOVEMENT_WINDOW;
                if (Clock::now() < movement_end + shared::TICK) {
                    return false;
                }
                captureS1PresentationEndpoint(Clock::now());
                uint64_t submitted_movement_frames = 0U;
                for (GameBenchmarkFrameSample const& frame : benchmark.raw_frames) {
                    if (frame.submitted_frame_phase == S1_MOVEMENT_FRAME_PHASE
                        && frame.submitted_frame_sequence_id) {
                        ++submitted_movement_frames;
                    }
                }
                uint64_t completed_movement_frames = 0U;
                for (GameBenchmarkFrameCompletion const& completion : benchmark.raw_fence_confirmed_frames) {
                    if (completion.submission_phase == S1_MOVEMENT_FRAME_PHASE) {
                        ++completed_movement_frames;
                    }
                }
                bool const pending_movement_frame = frame_completion_tracker.hasPendingPhase(
                    S1_MOVEMENT_FRAME_PHASE
                );
                s1_movement_window_drained = submitted_movement_frames != 0U
                    && submitted_movement_frames == completed_movement_frames
                    && !pending_movement_frame;
                if (!benchmark.presentation_timing || !benchmark.presentation_timing->capture_endpoint) {
                    return true;
                }
                bool const drain_timed_out = presentation_drain_attempts >= 120U
                    || (presentation_drain_started_at
                        && Clock::now() - *presentation_drain_started_at >= std::chrono::seconds{ 1 });
                if (drain_timed_out) {
                    return true;
                }
                return s1_movement_window_drained && benchmark.presentation_timing->drain_complete;
            }
            if (full_radius_workload) {
                GameBenchmarkFullRadiusPhase const phase = full_radius_sequence.phase();
                if (phase == GameBenchmarkFullRadiusPhase::Failed) {
                    return true;
                }
                if (phase != GameBenchmarkFullRadiusPhase::Complete
                    || !benchmark.presentation_timing) {
                    return phase == GameBenchmarkFullRadiusPhase::Complete;
                }
                return !benchmark.presentation_timing->capture_endpoint
                    || benchmark.presentation_timing->drain_complete
                    || presentation_drain_attempts >= 120U
                    || (presentation_drain_started_at
                        && Clock::now() - *presentation_drain_started_at >= std::chrono::seconds{ 1 });
            }
            Clock::time_point const measurement_end = *first_loop_at + options.cold_duration
                + options.warm_duration + options.uncapped_duration;
            Clock::time_point const stop_at = measurement_end
                + (options.workload == GameBenchmarkWorkload::Speed200Movement
                    ? GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE
                    : std::chrono::steady_clock::duration::zero());
            return Clock::now() >= stop_at;
        },
        .is_uncapped_phase = [&] {
            if (s1_terrain_workload) {
                std::optional<Clock::time_point> const movement_started_at = s1_movement_window.startedAt();
                return movement_started_at
                    && Clock::now() >= *movement_started_at
                    && Clock::now() < *movement_started_at + GAME_BENCHMARK_S1_MOVEMENT_WINDOW;
            }
            if (full_radius_workload || !first_loop_at) {
                return false;
            }
            Clock::time_point const uncapped_start = *first_loop_at
                + options.cold_duration + options.warm_duration;
            Clock::time_point const measurement_end = uncapped_start + options.uncapped_duration;
            return Clock::now() >= uncapped_start && Clock::now() < measurement_end;
        },
        .input_override = [
            &options,
            &first_loop_at,
            &movement_route_started_at,
            &s1_movement_window,
            &full_radius_sequence,
            &pending_movement_route,
            &previous_authoritative_player,
            &latest_s1_authoritative_state,
            &benchmark,
            &player,
            full_radius_workload,
            s1_terrain_workload
        ](
            uint64_t const ordinal
        )
            -> std::optional<shared::Direction> {
            if (full_radius_workload) {
                pending_movement_route.reset();
                if (full_radius_sequence.phase() != GameBenchmarkFullRadiusPhase::Movement) {
                    return shared::Direction{};
                }
                return gameBenchmarkDirection(options.workload, ordinal);
            }
            if (s1_terrain_workload) {
                Clock::time_point const now = Clock::now();
                client::WorldLoadingStatus const loading_status = player.worldLoadingStatus();
                bool const playable_sphere_ready_observed = loading_status.playable_sphere_first_ready_at
                    && *loading_status.playable_sphere_first_ready_at <= now;
                bool const current_sphere_ready = s1PlayableSphereReady(player, loading_status)
                    && playable_sphere_ready_observed;
                bool trajectory_anchor_ready = false;
                std::optional<Clock::time_point> const origin = first_loop_at
                    ? first_loop_at : loading_status.start_acknowledged_at;
                std::optional<uint64_t> const sample_offset = benchmarkOffsetFromStart(origin, now);
                if (current_sphere_ready && !s1_movement_window.startedAt()
                    && latest_s1_authoritative_state && sample_offset) {
                    auto anchor = gameBenchmarkS1TrajectoryAnchorSample(
                        *latest_s1_authoritative_state,
                        playableSphereSample(
                            loading_status, player.activeCoverageEpoch(), player.activeCoverageCenter(),
                            loading_status.actual_chunk_radius_blocks
                        ),
                        gameBenchmarkVisualLodSample(player, loading_status, player.benchmarkRuntimeInfo()),
                        *sample_offset
                    );
                    if (anchor && gameBenchmarkS1VisualLodSampleReady(anchor->visual_lod)) {
                        benchmark.s1_terrain_performance->movement_trajectory_anchor = std::move(anchor);
                        benchmark.s1_terrain_performance->movement_trajectory_anchor_sphere_sample_offset_ns =
                            sample_offset;
                        trajectory_anchor_ready = true;
                    }
                }
                if (s1_movement_window.shouldPauseBeforeStart(
                    playable_sphere_ready_observed,
                    current_sphere_ready,
                    trajectory_anchor_ready
                )) {
                    pending_movement_route.reset();
                    return shared::Direction{};
                }
                std::chrono::nanoseconds const elapsed = s1_movement_window.routeElapsedAt(
                    now,
                    movement_route_started_at,
                    current_sphere_ready
                );
                if (std::optional<Clock::time_point> const movement_started_at = s1_movement_window.startedAt();
                    movement_started_at && now >= *movement_started_at
                        + GAME_BENCHMARK_S1_MOVEMENT_WINDOW) {
                    pending_movement_route.reset();
                    return shared::Direction{};
                }
                GameBenchmarkMovementRouteSample route = gameBenchmarkRepeatingMovementRouteAt(elapsed);
                std::optional<double> player_z;
                std::optional<uint16_t> corridor_highest_solid_z;
                if (previous_authoritative_player) {
                    player_z = shared::playerPositionZ(*previous_authoritative_player);
                    double const player_x = shared::playerPositionX(*previous_authoritative_player);
                    double const player_y = shared::playerPositionY(*previous_authoritative_player);
                    shared::Direction const horizontal_direction = gameBenchmarkDirection(route);
                    double direction_x = static_cast<double>(std::bit_cast<int8_t>(horizontal_direction.x));
                    double direction_y = static_cast<double>(std::bit_cast<int8_t>(horizontal_direction.y));
                    double const horizontal_length = std::hypot(direction_x, direction_y);
                    if (horizontal_length > 0.0) {
                        direction_x /= horizontal_length;
                        direction_y /= horizontal_length;
                    }
                    static constexpr std::array<double, 9U> CLEARANCE_SAMPLE_DISTANCES{
                        0.0, 8.0, 16.0, 24.0, 32.0, 40.0, 48.0, 56.0, 64.0,
                    };
                    std::optional<shared::TerrainIdentity> const terrain_identity = player.activeTerrainIdentity();
                    bool complete_corridor = terrain_identity.has_value();
                    for (double const distance : CLEARANCE_SAMPLE_DISTANCES) {
                        std::optional<uint16_t> const surface_height = gameBenchmarkDeliveredSurfaceHeight(
                            player.heightTileResidency(),
                            terrain_identity ? &*terrain_identity : nullptr,
                            player_x + direction_x * distance,
                            player_y + direction_y * distance
                        );
                        if (!surface_height) {
                            complete_corridor = false;
                            break;
                        }
                        corridor_highest_solid_z = corridor_highest_solid_z
                            ? std::optional<uint16_t>{std::max(*corridor_highest_solid_z, *surface_height)}
                            : surface_height;
                    }
                    if (!complete_corridor) {
                        corridor_highest_solid_z.reset();
                    }
                }
                route = gameBenchmarkApplyFlightClearance(
                    route,
                    player_z,
                    corridor_highest_solid_z
                );
                pending_movement_route = route;
                return gameBenchmarkS1MovementDirection(
                    route,
                    current_sphere_ready || s1_movement_window.startedAt().has_value()
                );
            }
            pending_movement_route.reset();
            if (options.workload == GameBenchmarkWorkload::Speed200Movement && first_loop_at
                && Clock::now() >= *first_loop_at + options.cold_duration
                    + options.warm_duration + options.uncapped_duration) {
                return shared::Direction{};
            }
            return gameBenchmarkDirection(options.workload, ordinal);
        },
        .start_input_override = [&](uint64_t const ordinal) {
            GameBenchmarkStartActionEvidence& evidence = *benchmark.controlled_start;
            if (evidence.synthetic_input_override_invoked) {
                return false;
            }
            client::WorldLoadingStatus const status = player.worldLoadingStatus();
            if (status.state != client::WorldLoadingState::ReadyForStart
                || !status.ready_prompt_frame_rendered_at) {
                return false;
            }
            observeControlledStart(status);
            evidence.synthetic_input_override_invoked = true;
            evidence.synthetic_input_requested_offset_ns = offsetFromRequestAccepted(Clock::now());
            evidence.input_ordinal = ordinal;
            return true;
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            uint64_t const current_loop_ordinal = ++loop_ordinal;
            client::RendererRuntimeInfo const& runtime = loop_scratch->runtime.replaceFrom(
                [&] { return player.benchmarkRuntimeInfo(); }
            );
            for (; retired_frame_completions_consumed < runtime.retired_frame_completions.size();
                ++retired_frame_completions_consumed) {
                client::RendererRetiredFrameCompletion const& retired =
                    runtime.retired_frame_completions[retired_frame_completions_consumed];
                std::optional<uint64_t> const observed_offset = offsetFromBenchmarkStart(retired.observed_at);
                if (!observed_offset) {
                    continue;
                }
                auto completion = frame_completion_tracker.confirmFence(
                    retired.submission.swapchain_generation, retired.frame_slot, *observed_offset
                );
                if (completion) {
                    benchmark.raw_fence_confirmed_frames.push_back(std::move(completion).value());
                }
            }
            frame_completion_tracker.retireOtherGenerations(runtime.current_swapchain_generation);
            captureFenceConfirmedFrame(runtime, sample.completed_at);
            captureS1PresentationEndpoint(sample.completed_at);
            client::WorldLoadingStatus const& observed_loading_status = loop_scratch->loading_status.replaceFrom(
                [&] { return player.worldLoadingStatus(); }
            );
            observeControlledStart(observed_loading_status);
            observeS1LoadingStatus(observed_loading_status, sample.completed_at);
            if (!first_loop_at
                && sample.completed_at - last_loading_progress_at >= std::chrono::seconds{ 5 }) {
                last_loading_progress_at = sample.completed_at;
                std::cout << "[benchmark-game] loading state=" << worldLoadingStateName(observed_loading_status.state)
                    << " initial=" << observed_loading_status.initial_sphere_ready_chunks
                    << '/' << observed_loading_status.initial_sphere_required_chunks
                    << " playable=" << observed_loading_status.playable_sphere_ready_chunks
                    << '/' << observed_loading_status.playable_sphere_required_chunks
                    << " spawn_safe=" << observed_loading_status.spawn_safe
                    << " prompt_completed=" << observed_loading_status.ready_prompt_frame_rendered_at.has_value()
                    << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                        sample.completed_at - started_at
                    ).count() << '\n' << std::flush;
            }
            if (benchmark.presentation_timing && benchmark.presentation_timing->capture_endpoint) {
                ++presentation_drain_attempts;
                observePresentationGeneration(runtime, true);
                std::string_view const drain_phase = s1_terrain_workload
                    ? "s1-post-window-tail" : "recovered-stationary";
                static_cast<void>(captureSuccessfulRequest(
                    sample,
                    runtime,
                    drain_phase,
                    current_loop_ordinal,
                    true
                ));
                static_cast<void>(recordFrameSubmission(sample, runtime, drain_phase));
                pollPresentationHistory(GameBenchmarkPresentationPollScope::Drain);
                updatePresentationDrainCompletion();
                return;
            }
            if (!first_loop_at) {
                if (observed_loading_status.state != client::WorldLoadingState::Playing
                    || !observed_loading_status.start_acknowledged_at) {
                    return;
                }
                first_loop_at = *observed_loading_status.start_acknowledged_at;
                first_loop_started_at_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        first_loop_at->time_since_epoch()
                    ).count(),
                    std::memory_order_release
                );
                if (full_radius_workload) {
                    full_radius_sequence.start(*first_loop_at);
                    last_full_progress_at = *first_loop_at;
                    std::cout << "[benchmark-game] full-radius initial warmup started; timeout_ms="
                        << benchmark.full_radius_terrain->warmup_timeout_ms << '\n' << std::flush;
                }
                measurement_end_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        (*first_loop_at + options.cold_duration + options.warm_duration
                            + options.uncapped_duration).time_since_epoch()
                    ).count(),
                    std::memory_order_release
                );
                if (!full_radius_workload) {
                    std::cout << "[benchmark-game] connected; cold interval started\n" << std::flush;
                }
            }
            if (sample.completed_at <= *first_loop_at
                || (sample.started_at < *first_loop_at && !s1_terrain_workload)) {
                return;
            }
            Clock::time_point const sample_started_at = std::max(sample.started_at, *first_loop_at);
            GameBenchmarkPhase const phase = gameBenchmarkPhaseAt(sample.completed_at, *first_loop_at, options);
            if (!full_radius_workload && phase == GameBenchmarkPhase::Warm && reported_phase == 0U) {
                reported_phase = 1U;
                std::cout << "[benchmark-game] warm interval started\n" << std::flush;
            } else if (!full_radius_workload && phase == GameBenchmarkPhase::Uncapped && reported_phase < 2U) {
                reported_phase = 2U;
                std::cout << "[benchmark-game] uncapped interval started\n" << std::flush;
            }
            GameBenchmarkFullRadiusPhase const full_phase = full_radius_sequence.phase();
            std::optional<std::array<GameBenchmarkFrameSample::TerrainRadialBand, 16U>> terrain_radial_bands;
            std::optional<GameBenchmarkFrameSample::MeshEpisodes> mesh_episodes;
            uint64_t terrain_spatial_scan_duration_ns = 0U;
            std::chrono::nanoseconds const duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                sample.completed_at - sample_started_at
            );
            if (!full_radius_workload && phase == GameBenchmarkPhase::Uncapped) {
                if (sample.presentation_succeeded) {
                    uncapped_durations.push_back(duration);
                }
                uncapped_successful_present_requests += sample.presentation_succeeded ? 1U : 0U;
            } else if (!full_radius_workload && phase == GameBenchmarkPhase::Warm) {
                if (sample.presentation_succeeded) {
                    warm_durations.push_back(duration);
                }
                warm_successful_present_requests += sample.presentation_succeeded ? 1U : 0U;
            } else if (!full_radius_workload && phase == GameBenchmarkPhase::Cold) {
                if (sample.presentation_succeeded) {
                    cold_durations.push_back(duration);
                }
            }
            if (!full_radius_workload && phase == GameBenchmarkPhase::Outside) {
                ++benchmark.excluded_present_requests;
            } else {
                benchmark.successful_present_requests += sample.presentation_succeeded ? 1U : 0U;
                benchmark.failed_present_requests += sample.presentation_succeeded ? 0U : 1U;
            }
            if (!s1_terrain_workload) {
                benchmark.inputs_sent += sample.input_sent ? 1U : 0U;
            }
            std::optional<GameBenchmarkPresentationRequestId> presentation_request;
            if (benchmark.presentation_timing) {
                std::string_view const presentation_phase = s1_terrain_workload
                    ? s1FramePhaseAt(s1_movement_window, sample.completed_at)
                    : fullRadiusPhaseName(full_phase);
                observePresentationGeneration(
                    runtime,
                    (full_radius_workload && full_phase != GameBenchmarkFullRadiusPhase::InitialWarmup)
                        || (s1_terrain_workload && presentation_phase == S1_MOVEMENT_FRAME_PHASE)
                );
                presentation_request = captureSuccessfulRequest(
                    sample,
                    runtime,
                    presentation_phase,
                    current_loop_ordinal,
                    false
                );
                pollPresentationHistory(GameBenchmarkPresentationPollScope::Loop);
            }
            client::PlayerClientRenderDiagnostics const& client_render = loop_scratch->render_diagnostics.replaceFrom(
                [&] { return player.benchmarkRenderDiagnostics(); }
            );
            client::WorldLoadingStatus const& world_loading_status = observed_loading_status;
            client::PlayerClientBenchmarkFullTerrainInfo& full_terrain_info =
                loop_scratch->full_terrain_info.replaceFrom([&] {
                    if (full_radius_workload) {
                        return player.benchmarkFullTerrainStreamingInfo();
                    }
                    return client::PlayerClientBenchmarkFullTerrainInfo{};
                });
            std::optional<GameBenchmarkFullTerrainSnapshot>& full_radius_snapshot =
                loop_scratch->full_radius_snapshot.replaceFrom([] {
                    return std::optional<GameBenchmarkFullTerrainSnapshot>{};
                });
            if (full_radius_workload) {
                Clock::time_point observation_at = sample.completed_at;
                Clock::duration elapsed = full_radius_sequence.phaseElapsed(observation_at);
                uint64_t elapsed_ms = positiveNanoseconds(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                ) / 1'000'000U;
                GameBenchmarkFullRadiusEvidence& full_evidence = *benchmark.full_radius_terrain;
                std::optional<bool> exact_coverage;
                bool const is_warmup = full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                    || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup;
                if (full_phase == GameBenchmarkFullRadiusPhase::BaselineStationary) {
                    full_evidence.baseline_stationary_elapsed_ms = elapsed_ms;
                    baseline_full_radius_durations.push_back(duration);
                    if (sample.presentation_succeeded) {
                        ++full_evidence.baseline_present_requests;
                    }
                } else if (full_phase == GameBenchmarkFullRadiusPhase::Movement) {
                    full_evidence.movement_elapsed_ms = elapsed_ms;
                    if (full_terrain_info.center) {
                        if (!full_evidence.movement_start_center_valid) {
                            full_evidence.movement_start_center_valid = true;
                            full_evidence.movement_start_center_x = full_terrain_info.center->x;
                            full_evidence.movement_start_center_y = full_terrain_info.center->y;
                        }
                        if (last_movement_center && *last_movement_center != *full_terrain_info.center) {
                            ++full_evidence.movement_center_change_count;
                        }
                        last_movement_center = full_terrain_info.center;
                        full_evidence.movement_end_center_valid = true;
                        full_evidence.movement_end_center_x = full_terrain_info.center->x;
                        full_evidence.movement_end_center_y = full_terrain_info.center->y;
                    }
                } else if (full_phase == GameBenchmarkFullRadiusPhase::RecoveredStationary) {
                    full_evidence.recovered_stationary_elapsed_ms = elapsed_ms;
                    recovered_full_radius_durations.push_back(duration);
                    if (sample.presentation_succeeded) {
                        ++full_evidence.recovered_present_requests;
                    }
                }

                bool* candidate_scanned = full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                    ? &full_initial_candidate_scanned
                    : &full_recovery_candidate_scanned;
                bool const timeout_reached = is_warmup
                    && elapsed >= options.full_radius_warmup_timeout;
                if (is_warmup && !full_terrain_info.potentially_ready) {
                    *candidate_scanned = false;
                }
                bool const spatial_phase = full_phase == GameBenchmarkFullRadiusPhase::Movement
                    || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup;
                bool const spatial_interval_elapsed = !last_full_spatial_scan_at
                    || sample.completed_at - *last_full_spatial_scan_at >= std::chrono::seconds{ 5 }
                    || last_full_spatial_scan_phase != full_phase;
                if (full_radius_spatial_scan_enabled && spatial_phase && spatial_interval_elapsed) {
                    std::chrono::steady_clock::time_point const spatial_scan_started_at = Clock::now();
                    full_terrain_info = player.benchmarkFullTerrainStreamingInfo(true);
                    terrain_spatial_scan_duration_ns = positiveNanoseconds(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - spatial_scan_started_at)
                    );
                    std::array<GameBenchmarkFrameSample::TerrainRadialBand, 16U> bands{};
                    for (uint32_t index = 0U;
                        index < client::PLAYER_CLIENT_BENCHMARK_FULL_TERRAIN_RADIAL_BAND_COUNT; ++index) {
                        auto const& source = full_terrain_info.radial_bands[index];
                        bands[index] = {
                            .desired_count = source.desired_count,
                            .resident_count = source.resident_count,
                            .nonresident_count = source.nonresident_count,
                            .current_mesh_visible_count = source.current_mesh_visible_count,
                            .stale_or_missing_mesh_count = source.stale_or_missing_mesh_count,
                        };
                    }
                    terrain_radial_bands = bands;
                    last_full_spatial_scan_at = sample.completed_at;
                    last_full_spatial_scan_phase = full_phase;
                    observation_at = Clock::now();
                    elapsed = full_radius_sequence.phaseElapsed(observation_at);
                    elapsed_ms = positiveNanoseconds(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    ) / 1'000'000U;
                }
                if (is_warmup
                    && full_terrain_info.scan_performed) {
                    exact_coverage = full_terrain_info.exact_current_coverage;
                    full_radius_snapshot = fullTerrainSnapshot(
                        full_terrain_info, runtime.stone_face_growth, world_loading_status, full_phase, elapsed
                    );
                    *candidate_scanned = full_terrain_info.potentially_ready;
                } else if (is_warmup
                    && ((full_terrain_info.potentially_ready && !*candidate_scanned) || timeout_reached)) {
                    full_terrain_info = player.benchmarkFullTerrainStreamingInfo(true);
                    observation_at = Clock::now();
                    elapsed = full_radius_sequence.phaseElapsed(observation_at);
                    elapsed_ms = positiveNanoseconds(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    ) / 1'000'000U;
                    exact_coverage = full_terrain_info.exact_current_coverage;
                    full_radius_snapshot = fullTerrainSnapshot(
                        full_terrain_info, runtime.stone_face_growth, world_loading_status, full_phase, elapsed
                    );
                    *candidate_scanned = full_terrain_info.potentially_ready;
                }
                if (full_terrain_info.mesh_episodes) {
                    auto const& source = *full_terrain_info.mesh_episodes;
                    mesh_episodes = GameBenchmarkFrameSample::MeshEpisodes{
                        .entered_count = source.entered_count,
                        .completed_count = source.completed_count,
                        .censored_count = source.censored_count,
                        .completed_total_ns = source.completed_total_ns,
                        .completed_max_ns = source.completed_max_ns,
                        .censored_max_ns = source.censored_max_ns,
                        .tracked_count = source.tracked_count,
                        .pending_count = source.pending_count,
                        .oldest_pending_ns = source.oldest_pending_ns,
                        .oldest_resident_pending_ns = source.oldest_resident_pending_ns,
                        .completed_latency_buckets = source.completed_latency_buckets,
                        .censored_latency_buckets = source.censored_latency_buckets,
                        .never_resident_censored_count = source.never_resident_censored_count,
                        .resident_censored_count = source.resident_censored_count,
                        .resident_censored_max_wait_ns = source.resident_censored_max_wait_ns,
                        .never_resident_censored_entry_latency_buckets =
                            source.never_resident_censored_entry_latency_buckets,
                        .resident_censored_wait_latency_buckets = source.resident_censored_wait_latency_buckets,
                        .resident_at_departure_censored_entry_latency_buckets =
                            source.resident_at_departure_censored_entry_latency_buckets,
                        .nonresident_at_departure_censored_entry_latency_buckets =
                            source.nonresident_at_departure_censored_entry_latency_buckets,
                        .unknown_departure_residency_censored_count = source.unknown_departure_residency_censored_count,
                        .global_active_mesh_job_count = full_terrain_info.active_mesh_job_count,
                        .global_worker_result_count = full_terrain_info.worker_result_count,
                        .global_worker_outstanding_count = full_terrain_info.worker_outstanding_count,
                        .pending_by_current_band = source.pending_by_current_band,
                        .oldest_pending_ns_by_current_band = source.oldest_pending_ns_by_current_band,
                    };
                }
                if (full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup) {
                    full_evidence.initial_warmup_elapsed_ms = elapsed_ms;
                } else if (full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup) {
                    full_evidence.recovery_warmup_elapsed_ms = elapsed_ms;
                }

                GameBenchmarkFullRadiusPhase const previous_full_phase = full_phase;
                full_radius_sequence.observe(
                    observation_at, exact_coverage, playableSphereReady(world_loading_status)
                );
                GameBenchmarkFullRadiusPhase const next_full_phase = full_radius_sequence.phase();
                if (previous_full_phase == GameBenchmarkFullRadiusPhase::BaselineStationary
                    && next_full_phase == GameBenchmarkFullRadiusPhase::Movement) {
                    if (options.workload != GameBenchmarkWorkload::OrdinaryTurningMovement) {
                        full_radius_movement_started_at = observation_at;
                    }
                } else if (previous_full_phase == GameBenchmarkFullRadiusPhase::Movement
                    && next_full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup) {
                    full_radius_movement_window_complete = true;
                }
                if (previous_full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                    && next_full_phase != previous_full_phase) {
                    full_evidence.initial_barrier_reached = next_full_phase
                        == GameBenchmarkFullRadiusPhase::BaselineStationary;
                    full_evidence.initial_barrier = full_radius_snapshot.value_or(
                        fullTerrainSnapshot(
                            full_terrain_info,
                            runtime.stone_face_growth,
                            world_loading_status,
                            previous_full_phase,
                            elapsed
                        )
                    );
                } else if (previous_full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup
                    && next_full_phase != previous_full_phase) {
                    full_evidence.recovery_barrier_reached = next_full_phase
                        == GameBenchmarkFullRadiusPhase::RecoveredStationary;
                    full_evidence.recovery_barrier = full_radius_snapshot.value_or(
                        fullTerrainSnapshot(
                            full_terrain_info,
                            runtime.stone_face_growth,
                            world_loading_status,
                            previous_full_phase,
                            elapsed
                        )
                    );
                }
                if (next_full_phase == GameBenchmarkFullRadiusPhase::Complete
                    || next_full_phase == GameBenchmarkFullRadiusPhase::Failed) {
                    full_radius_measurement_end_at = sample.completed_at;
                    if (next_full_phase == GameBenchmarkFullRadiusPhase::Complete
                        && benchmark.presentation_timing
                        && !benchmark.presentation_timing->successful_requests.empty()) {
                        GameBenchmarkSuccessfulPresentationRequest const& endpoint_request =
                            benchmark.presentation_timing->successful_requests.back();
                        benchmark.presentation_timing->capture_endpoint = GameBenchmarkPresentationCaptureEndpoint{
                            .request = endpoint_request.request,
                            .loop_ordinal = endpoint_request.loop_ordinal,
                        };
                        presentation_drain_started_at = sample.completed_at;
                        updatePresentationDrainCompletion();
                    }
                }
                if (next_full_phase != previous_full_phase) {
                    std::cout << "[benchmark-game] full-radius phase="
                        << fullRadiusPhaseName(next_full_phase)
                        << " elapsed_ms=" << elapsed_ms << '\n' << std::flush;
                    if (next_full_phase == GameBenchmarkFullRadiusPhase::BaselineStationary
                        || next_full_phase == GameBenchmarkFullRadiusPhase::RecoveredStationary) {
                        last_full_warmup_raw_at.reset();
                    }
                }

                if (is_warmup && last_full_progress_at
                    && sample.completed_at - *last_full_progress_at >= std::chrono::seconds{ 15 }) {
                    last_full_progress_at = sample.completed_at;
                    std::cout << "[benchmark-game] full-radius phase=" << fullRadiusPhaseName(full_phase)
                        << " elapsed_ms=" << elapsed_ms
                        << " timeout_ms=" << benchmark.full_radius_terrain->warmup_timeout_ms
                        << " desired=" << full_terrain_info.desired_count
                        << " resident_total=" << full_terrain_info.resident_total_count
                        << " visible_meshes=" << full_terrain_info.visible_mesh_count
                        << " renderer_slots=" << full_terrain_info.renderer_mesh_count
                        << " delivery_batches=" << full_terrain_info.pending_delivery_batch_count
                        << " residency_changes=" << full_terrain_info.pending_residency_change_count
                        << " pending_mesh=" << full_terrain_info.pending_mesh_count
                        << " active_mesh_jobs=" << full_terrain_info.active_mesh_job_count
                        << " worker_outstanding=" << full_terrain_info.worker_outstanding_count
                        << " pending_removals=" << full_terrain_info.pending_removal_count
                        << " stone_face_growth=" << stoneFaceGrowthDiagnosticsJson(runtime.stone_face_growth)
                        << '\n' << std::flush;
                }
            }
            std::optional<GameBenchmarkFrameSample::TerrainNearSample> terrain_near_sample;
            if (options.workload == GameBenchmarkWorkload::Speed200Movement
                || (full_radius_workload && (full_phase == GameBenchmarkFullRadiusPhase::Movement
                    || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup))) {
                client::PlayerClientBenchmarkTerrainInfo const& terrain_info =
                    loop_scratch->terrain_info.replaceFrom([&] { return player.benchmarkTerrainStreamingInfo(); });
                GameBenchmarkTerrainObservation const observation =
                    gameBenchmarkTerrainObservation(terrain_info);
                terrain_readiness_tracker.observe(observation, sample.completed_at);
                terrain_near_sample = gameBenchmarkTerrainFrameSample(
                    terrain_info,
                    terrain_info.accepted_near_admissions >= last_accepted_near_admissions
                        ? terrain_info.accepted_near_admissions - last_accepted_near_admissions
                        : terrain_info.accepted_near_admissions,
                    terrain_info.accepted_far_admissions >= last_accepted_far_admissions
                        ? terrain_info.accepted_far_admissions - last_accepted_far_admissions
                        : terrain_info.accepted_far_admissions,
                    terrain_info.far_admissions_while_near_pending >= last_far_admissions_while_near_pending
                        ? terrain_info.far_admissions_while_near_pending - last_far_admissions_while_near_pending
                        : terrain_info.far_admissions_while_near_pending
                );
                last_accepted_near_admissions = terrain_info.accepted_near_admissions;
                last_accepted_far_admissions = terrain_info.accepted_far_admissions;
                last_far_admissions_while_near_pending = terrain_info.far_admissions_while_near_pending;
            }
            bool record_raw_frame = !s1_terrain_workload
                || s1FramePhaseAt(s1_movement_window, sample.completed_at) != "s1-post-window-tail";
            if (full_radius_workload
                && (full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                    || (full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup
                        && options.qualification_proof.empty()))) {
                record_raw_frame = !last_full_warmup_raw_at
                    || sample.completed_at - *last_full_warmup_raw_at >= std::chrono::seconds{ 1 };
                if (record_raw_frame) {
                    last_full_warmup_raw_at = sample.completed_at;
                }
            }
            std::string const frame_submission_phase = s1_terrain_workload
                ? std::string{ s1FramePhaseAt(s1_movement_window, sample.completed_at) }
                : full_radius_workload
                    ? std::string{ fullRadiusPhaseName(full_phase) }
                    : std::string{};
            std::optional<uint64_t> const frame_submission_sequence_id = recordFrameSubmission(
                sample,
                runtime,
                frame_submission_phase
            );
            std::optional<uint32_t> const frame_submission_slot = runtime.diagnostic_presentation_acquire.captured
                && runtime.diagnostic_presentation_acquire.frame_slot
                    < GameBenchmarkFrameCompletionTracker::FRAME_SLOT_COUNT
                ? std::optional<uint32_t>{ runtime.diagnostic_presentation_acquire.frame_slot }
                : std::nullopt;
            uint64_t const frame_submission_offset_ns = frame_submission_sequence_id
                ? offsetFromBenchmarkStart(sample.completed_at).value_or(0U) : 0U;
            bool const full_radius_warmup_frame_excluded = !options.qualification_proof.empty()
                && (full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                    || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup);
            bool const frame_excluded = s1_terrain_workload
                ? frame_submission_phase != S1_MOVEMENT_FRAME_PHASE
                : full_radius_workload
                    ? full_radius_warmup_frame_excluded
                    : phase == GameBenchmarkPhase::Outside;
            std::optional<GameBenchmarkFrameSample::PlayableSphere> playable_sphere;
            if (full_radius_workload || s1_terrain_workload) {
                uint32_t const radius_blocks = s1_terrain_workload
                    ? world_loading_status.actual_chunk_radius_blocks
                    : shared::MAX_SPHERE_RADIUS_BLOCKS;
                playable_sphere = playableSphereSample(
                    world_loading_status,
                    player.activeCoverageEpoch(),
                    player.activeCoverageCenter(),
                    radius_blocks,
                    player.authoritativeWorld()
                );
            }
            std::optional<GameBenchmarkFrameSample::VisualLod> visual_lod;
            if (s1_terrain_workload) {
                visual_lod = gameBenchmarkVisualLodSample(player, world_loading_status, runtime);
            }
            std::optional<client::PlayerClientBenchmarkPresentationInfo> const presentation_info = s1_terrain_workload
                ? player.benchmarkPresentationInfo()
                : std::nullopt;
            std::optional<GameBenchmarkFrameSample::QualificationSettings> qualification_settings;
            if (s1_terrain_workload) {
                qualification_settings = GameBenchmarkFrameSample::QualificationSettings{
                    .framebuffer_width = runtime.width,
                    .framebuffer_height = runtime.height,
                    .render_distance = options.render_distance,
                    .debug_hud_enabled = runtime.debug_hud_enabled,
                    .terrain_model = std::string{shared::TERRAIN_LOD_MODEL},
                    .camera_perspective = presentation_info
                        && presentation_info->perspective == client::CameraPerspective::FirstPerson
                            ? "first-person" : "other",
                    .world_mode = presentation_info
                        && presentation_info->world_mode == shared::WorldMode::Flight
                            ? "flight" : "other",
                };
            }
            std::optional<GameBenchmarkFrameSample::PresentedPlayer> presented_player;
            if (s1_terrain_workload && presentation_info) {
                presented_player = GameBenchmarkFrameSample::PresentedPlayer{
                    .body = {
                        presentation_info->body.x,
                        presentation_info->body.y,
                        presentation_info->body.z,
                    },
                    .submitted_frame_count = presentation_info->submitted_frame_count,
                    .sampled_offset_ns = positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        presentation_info->sampled_at - *first_loop_at
                    )),
                };
            }
            GameBenchmarkFrameSample& frame_sample = loop_scratch->frame_sample.replaceWith([&](void* storage) {
                return ::new (storage) GameBenchmarkFrameSample{
                    .loop_ordinal = current_loop_ordinal,
                    .warm = !full_radius_workload && !s1_terrain_workload && phase == GameBenchmarkPhase::Warm,
                    .uncapped = !full_radius_workload && !s1_terrain_workload
                        && phase == GameBenchmarkPhase::Uncapped,
                    .excluded = frame_excluded,
                    .presentation_succeeded = sample.presentation_succeeded,
                    .swapchain_generation = sample.presentation_succeeded
                        ? std::optional<uint64_t>{ runtime.successful_presentation_swapchain_generation } : std::nullopt,
                    .application_present_id = sample.presentation_succeeded
                        ? runtime.successful_presentation_id : std::nullopt,
                    .submitted_frame_sequence_id = frame_submission_sequence_id,
                    .submitted_frame_slot = frame_submission_slot,
                    .submitted_frame_offset_ns = frame_submission_offset_ns,
                    .submitted_frame_phase = frame_submission_phase,
                    .started_offset_ns = positiveNanoseconds(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(sample_started_at - *first_loop_at)
                    ),
                    .loop_duration_ns = positiveNanoseconds(duration),
                    .render_duration_ns = positiveNanoseconds(sample.render_duration),
                    .network_poll_duration_ns = positiveNanoseconds(sample.network_poll_duration),
                    .height_tile_delivery_duration_ns = positiveNanoseconds(sample.height_tile_delivery_duration),
                    .network_event_count = sample.network_event_count,
                    .client_message_payload_bytes_sent = sample.client_message_payload_bytes_sent,
                    .client_message_payload_bytes_received = sample.client_message_payload_bytes_received,
                    .resident_terrain_tile_count = runtime.height_tile_mesh_count,
                    .visible_surface_face_count = runtime.chunk_face_count,
                    .terrain_phase = s1_terrain_workload
                        ? frame_submission_phase
                        : full_radius_workload
                            ? std::string{ fullRadiusPhaseName(full_phase) } : std::string{},
                    .terrain_center_valid = full_radius_workload && full_terrain_info.center.has_value(),
                    .terrain_center_x = full_radius_workload && full_terrain_info.center
                        ? full_terrain_info.center->x : 0,
                    .terrain_center_y = full_radius_workload && full_terrain_info.center
                        ? full_terrain_info.center->y : 0,
                    .playable_sphere = std::move(playable_sphere),
                    .visual_lod = std::move(visual_lod),
                    .qualification_settings = std::move(qualification_settings),
                    .presented_player = presented_player,
                    .terrain_near = terrain_near_sample,
                    .diagnostic_terrain_radial_bands = terrain_radial_bands,
                    .diagnostic_mesh_episodes = mesh_episodes,
                    .diagnostic_terrain_spatial_scan_duration_ns = terrain_spatial_scan_duration_ns,
                    .renderer_stone_face_growth = record_raw_frame
                        && (s1_terrain_workload || (full_radius_workload && (
                            full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                            || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup
                        )))
                        ? std::optional{ runtime.stone_face_growth } : std::nullopt,
                    .diagnostic_render_attempt_id = runtime.diagnostic_render_attempt_id,
                    .diagnostic_frame_recorded = runtime.diagnostic_frame_recorded,
                    .diagnostic_indexed_stone_quads = runtime.diagnostic_indexed_stone_quads,
                    .diagnostic_stone_indirect = runtime.diagnostic_stone_indirect,
                    .diagnostic_frame_slot = runtime.diagnostic_frame_slot,
                    .diagnostic_submitted_stone_quad_count = runtime.diagnostic_submitted_stone_quad_count,
                    .diagnostic_stone_draw_count = runtime.diagnostic_stone_draw_count,
                    .diagnostic_camera = runtime.diagnostic_camera,
                    .diagnostic_cpu_acquire_wait_duration_ns = positiveNanoseconds(runtime.cpu_acquire_wait_duration),
                    .diagnostic_presentation_acquire = gameBenchmarkPresentationAcquireSample(
                        runtime.diagnostic_presentation_acquire
                    ),
                    .diagnostic_cpu_presentation_recreation_duration_ns = positiveNanoseconds(
                        runtime.cpu_presentation_recreation_duration
                    ),
                    .diagnostic_presentation_recreation_attempted = runtime.presentation_recreation_attempted,
                    .diagnostic_presentation_recreation_deferred = runtime.presentation_recreation_deferred,
                    .diagnostic_cpu_command_record_duration_ns = positiveNanoseconds(runtime.cpu_command_record_duration),
                    .diagnostic_cpu_visible_range_collection_duration_ns = positiveNanoseconds(
                        runtime.cpu_visible_range_collection_duration
                    ),
                    .diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns = positiveNanoseconds(
                        runtime.cpu_height_tile_group_and_tile_traversal_duration
                    ),
                    .diagnostic_cpu_height_tile_pending_range_order_duration_ns = positiveNanoseconds(
                        runtime.cpu_height_tile_pending_range_order_duration
                    ),
                    .diagnostic_cpu_height_tile_visible_range_merge_duration_ns = positiveNanoseconds(
                        runtime.cpu_height_tile_visible_range_merge_duration
                    ),
                    .diagnostic_cpu_indirect_prepare_duration_ns = positiveNanoseconds(runtime.cpu_indirect_prepare_duration),
                    .diagnostic_cpu_stone_scene_record_duration_ns = positiveNanoseconds(
                        runtime.cpu_stone_scene_record_duration
                    ),
                    .diagnostic_cpu_command_record_residual_duration_ns = positiveNanoseconds(
                        runtime.cpu_command_record_residual_duration
                    ),
                    .diagnostic_cpu_complete_present_wait_duration_ns = positiveNanoseconds(
                        runtime.cpu_complete_present_wait_duration
                    ),
                    .diagnostic_client_window_input_duration_ns = positiveNanoseconds(
                        client_render.window_input_duration
                    ),
                    .diagnostic_client_height_tile_interest_duration_ns = positiveNanoseconds(
                        client_render.height_tile_interest_duration
                    ),
                    .diagnostic_client_preview_lod_refresh_duration_ns = positiveNanoseconds(
                        client_render.preview_lod_refresh_duration
                    ),
                    .diagnostic_client_preview_lod_classified_bucket_count =
                        client_render.preview_lod_classified_bucket_count,
                    .diagnostic_client_preview_lod_evaluated_tile_count =
                        client_render.preview_lod_evaluated_tile_count,
                    .diagnostic_client_preview_lod_changed_tile_count =
                        client_render.preview_lod_changed_tile_count,
                    .diagnostic_client_preview_mesh_pump_before_changes_duration_ns = positiveNanoseconds(
                        client_render.preview_mesh_pump_before_changes_duration
                    ),
                    .diagnostic_client_height_tile_change_and_removal_duration_ns = positiveNanoseconds(
                        client_render.height_tile_change_and_removal_duration
                    ),
                    .diagnostic_client_height_tile_change_queue_duration_ns = positiveNanoseconds(
                        client_render.height_tile_change_queue_duration
                    ),
                    .diagnostic_client_preview_mesh_remove_duration_ns = positiveNanoseconds(
                        client_render.preview_mesh_remove_duration
                    ),
                    .diagnostic_client_preview_mesh_remove_renderer_duration_ns = positiveNanoseconds(
                        client_render.preview_mesh_remove_renderer_duration
                    ),
                    .diagnostic_client_preview_mesh_remove_neighbor_enqueue_duration_ns = positiveNanoseconds(
                        client_render.preview_mesh_remove_neighbor_enqueue_duration
                    ),
                    .diagnostic_client_preview_mesh_remove_count = client_render.preview_mesh_remove_count,
                    .diagnostic_renderer_height_tile_remove_prefilter_duration_ns = positiveNanoseconds(
                        runtime.diagnostic_height_tile_removal.prefilter_duration
                    ),
                    .diagnostic_renderer_height_tile_remove_slot_retirement_duration_ns = positiveNanoseconds(
                        runtime.diagnostic_height_tile_removal.slot_retirement_duration
                    ),
                    .diagnostic_renderer_height_tile_remove_draw_index_duration_ns = positiveNanoseconds(
                        runtime.diagnostic_height_tile_removal.draw_index_remove_duration
                    ),
                    .diagnostic_renderer_height_tile_remove_input_count =
                        runtime.diagnostic_height_tile_removal.input_coordinate_count,
                    .diagnostic_renderer_height_tile_remove_matching_slot_count =
                        runtime.diagnostic_height_tile_removal.matching_slot_count,
                    .diagnostic_renderer_height_tile_remove_actual_slot_count =
                        runtime.diagnostic_height_tile_removal.removed_slot_count,
                    .diagnostic_client_preview_mesh_pump_after_changes_duration_ns = positiveNanoseconds(
                        client_render.preview_mesh_pump_after_changes_duration
                    ),
                    .diagnostic_client_camera_resolution_duration_ns = positiveNanoseconds(
                        client_render.camera_resolution_duration
                    ),
                    .diagnostic_client_render_before_resize_duration_ns = positiveNanoseconds(
                        client_render.before_resize_duration
                    ),
                    .diagnostic_renderer_resize_duration_ns = positiveNanoseconds(client_render.resize_duration),
                    .diagnostic_client_render_after_resize_duration_ns = positiveNanoseconds(
                        client_render.after_resize_duration
                    ),
                    .diagnostic_renderer_call_duration_ns = positiveNanoseconds(client_render.renderer_call_duration),
                    .diagnostic_client_render_after_renderer_duration_ns = positiveNanoseconds(
                        client_render.after_renderer_duration
                    ),
                    .diagnostic_gpu_timestamps_enabled = runtime.diagnostic_gpu_timestamps_enabled,
                    .diagnostic_gpu_timestamp_reason = runtime.diagnostic_gpu_timestamp_reason,
                    .diagnostic_gpu_sample_attempt_id = runtime.diagnostic_gpu_sample_attempt_id,
                    .diagnostic_gpu_sample_slot = runtime.diagnostic_gpu_sample_slot,
                    .diagnostic_gpu_sample_quad_count = runtime.diagnostic_gpu_sample_quad_count,
                    .diagnostic_gpu_sample_draw_count = runtime.diagnostic_gpu_sample_draw_count,
                    .diagnostic_gpu_terrain_duration_ns = runtime.diagnostic_gpu_terrain_duration
                        ? std::optional<uint64_t>{ positiveNanoseconds(*runtime.diagnostic_gpu_terrain_duration) }
                        : std::nullopt,
                    .latest_gpu_frame_duration_ns = runtime.gpu_frame_duration
                        ? std::optional<uint64_t>{ positiveNanoseconds(*runtime.gpu_frame_duration) }
                        : std::nullopt,
                    .diagnostic_terrain_occlusion_enabled = runtime.diagnostic_terrain_occlusion_enabled,
                    .diagnostic_terrain_occlusion_oracle_frame = runtime.diagnostic_terrain_occlusion_oracle_frame,
                    .diagnostic_terrain_occlusion_filtered_frame = runtime.diagnostic_terrain_occlusion_filtered_frame,
                    .diagnostic_terrain_occlusion_removed_quad_count =
                        runtime.diagnostic_terrain_occlusion_removed_quad_count,
                    .diagnostic_terrain_occlusion_removed_range_count =
                        runtime.diagnostic_terrain_occlusion_removed_range_count,
                    .diagnostic_terrain_occlusion_removed_command_count =
                        runtime.diagnostic_terrain_occlusion_removed_command_count,
                };
            });
            if (record_raw_frame) {
                benchmark.raw_frames.push_back(std::move(frame_sample));
            }
        },
        .on_authoritative_player = [&](shared::Player const& authoritative_player) {
            if (full_radius_measurement_end_at && Clock::now() >= *full_radius_measurement_end_at) {
                return;
            }
            ++benchmark.authoritative_player_updates;
            if (previous_authoritative_player) {
                if (previous_authoritative_player->x > shared::World::FLIGHT_MAX_CELL - 256
                    && authoritative_player.x < 256) {
                    ++benchmark.observed_wrap_crossings;
                }
                if (previous_authoritative_player->movement_capabilities
                    != authoritative_player.movement_capabilities) {
                    ++benchmark.observed_capability_transitions;
                }
            }
            previous_authoritative_player = authoritative_player;
        },
        .on_input_sent = [&] (
            shared::ClientInputMessage const& sent_input,
            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): callback order separates these timestamps.
            Clock::time_point const sent_at,
            Clock::time_point const scheduled_at
        ) {
            if (!s1_terrain_workload) {
                pending_movement_route.reset();
                return;
            }
            client::WorldLoadingStatus const status = player.worldLoadingStatus();
            std::optional<Clock::time_point> const benchmark_origin = first_loop_at
                ? first_loop_at : status.start_acknowledged_at;
            if (!benchmark_origin || sent_at < *benchmark_origin) {
                pending_movement_route.reset();
                return;
            }
            GameBenchmarkMovementRouteSample const movement_intent = pending_movement_route.value_or(
                GameBenchmarkMovementRouteSample{}
            );
            bool const has_movement_intent = movement_intent.strafe != 0
                || movement_intent.forward != 0
                || movement_intent.vertical != 0;
            bool const has_movement_direction = sent_input.direction.x != 0U
                || sent_input.direction.y != 0U
                || sent_input.direction.z != 0U;
            bool const movement_input = pending_movement_route
                && has_movement_intent
                && has_movement_direction;
            bool const x30_route_input = movement_input && isX30MovementInput(sent_input.direction);
            bool const first_recording = movement_input_sequences.insert(sent_input.sequence).second;
            if (!first_recording) {
                pending_movement_route.reset();
                return;
            }
            ++benchmark.inputs_sent;
            if (movement_input && !movement_route_started_at) {
                movement_route_started_at = sent_at;
            }
            observeS1LoadingStatus(status, sent_at);
            bool const current_sphere_ready = s1PlayableSphereReady(player, status)
                && status.playable_sphere_first_ready_at
                && *status.playable_sphere_first_ready_at <= sent_at;
            if (movement_input && !current_sphere_ready) {
                benchmark.s1_terrain_performance->movement_input_during_fill = true;
            }
            bool const measurement_started = x30_route_input
                && s1_movement_window.observeSuccessfulInput(sent_at, current_sphere_ready);
            if (measurement_started) {
                GameBenchmarkS1TerrainPerformanceEvidence& evidence = *benchmark.s1_terrain_performance;
                evidence.movement_started_after_current_sphere_ready = true;
                evidence.movement_start_offset_ns = offsetFromRequestAccepted(sent_at).value_or(0U);
                evidence.movement_end_offset_ns = evidence.movement_start_offset_ns
                    + positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        GAME_BENCHMARK_S1_MOVEMENT_WINDOW
                    ));
                evidence.movement_boundary_authoritative_updates_complete = true;
            }
            bool const in_measurement_window = x30_route_input && s1_movement_window.contains(sent_at);
            std::optional<uint64_t> const offset = benchmarkOffsetFromStart(benchmark_origin, sent_at);
            std::optional<int64_t> const scheduled_offset = signedBenchmarkOffsetFromStart(
                benchmark_origin,
                scheduled_at
            );
            if (offset && scheduled_offset && first_recording) {
                benchmark.raw_gameplay_inputs.push_back(GameBenchmarkRawInputEvidence{
                    .input_ordinal = static_cast<uint64_t>(benchmark.raw_gameplay_inputs.size()) + 1U,
                    .sequence = sent_input.sequence,
                    .sent_offset_ns = *offset,
                    .scheduled_offset_ns = *scheduled_offset,
                    .movement_intent = movement_intent,
                    .direction = sent_input.direction,
                });
            }
            if (in_measurement_window) {
                s1_movement_input_sequences.insert(sent_input.sequence);
            } else if (!s1_movement_window.startedAt()) {
                s1_pre_window_input_sequences.insert(sent_input.sequence);
            } else if (sent_at >= *s1_movement_window.startedAt() + GAME_BENCHMARK_S1_MOVEMENT_WINDOW) {
                s1_post_window_input_sequences.insert(sent_input.sequence);
            }
            pending_movement_route.reset();
        },
        .on_authoritative_state = [&](
            shared::ServerPlayerPositionMessage const& server_position,
            shared::Player const& authoritative_player,
            Clock::time_point const observed_at
        ) {
            if (full_radius_workload
                && full_radius_measurement_end_at && observed_at >= *full_radius_measurement_end_at) {
                return;
            }
            if (!full_radius_workload && !s1_terrain_workload) {
                return;
            }
            uint64_t const update_ordinal = ++authoritative_state_ordinal;
            std::optional<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate> update =
                fullRadiusMovementObservation(
                    player,
                    authoritative_player,
                    server_position,
                    update_ordinal,
                    observed_at,
                    s1_terrain_workload && !first_loop_at
                        ? std::optional<Clock::time_point>{ observed_at } : first_loop_at,
                    s1_terrain_workload
                        ? shared::ACTUAL_TERRAIN_RADIUS_BLOCKS
                        : shared::MAX_SPHERE_RADIUS_BLOCKS
                );
            if (s1_terrain_workload && update) {
                latest_s1_authoritative_state = *update;
                update->visual_lod = gameBenchmarkVisualLodSample(
                    player,
                    player.worldLoadingStatus(),
                    player.benchmarkRuntimeInfo()
                );
            }
            if (!update) {
                if (s1_terrain_workload) {
                    std::optional<Clock::time_point> const movement_start = s1_movement_window.startedAt();
                    if (movement_start && observed_at >= *movement_start
                        && observed_at < *movement_start + GAME_BENCHMARK_S1_MOVEMENT_WINDOW + shared::TICK) {
                        benchmark.s1_terrain_performance->movement_boundary_authoritative_updates_complete = false;
                    }
                }
                return;
            }
            if (s1_terrain_workload) {
                GameBenchmarkS1TerrainPerformanceEvidence& s1_evidence = *benchmark.s1_terrain_performance;
                std::optional<Clock::time_point> const movement_start = s1_movement_window.startedAt();
                if (!movement_start || observed_at < *movement_start) {
                    if (!first_loop_at) {
                        return;
                    }
                    if (!s1_evidence.movement_trajectory_anchor
                        || update->offset_ns > s1_evidence.movement_trajectory_anchor->offset_ns) {
                        s1_evidence.movement_trajectory_anchor = *update;
                    }
                    return;
                }
                Clock::time_point const movement_end = *movement_start + GAME_BENCHMARK_S1_MOVEMENT_WINDOW;
                if (s1_post_window_input_sequences.contains(server_position.acknowledged_input_sequence)) {
                    return;
                }
                if (observed_at >= movement_end + shared::TICK) {
                    if (s1_pre_window_input_sequences.contains(server_position.acknowledged_input_sequence)) {
                        s1_evidence.movement_boundary_authoritative_updates_complete = false;
                    }
                    return;
                }
                if (s1_movement_input_sequences.contains(server_position.acknowledged_input_sequence)) {
                    ++s1_evidence.movement_authoritative_update_count;
                    s1_evidence.movement_authoritative_sphere_updates.push_back(*update);
                } else {
                    if (!s1_pre_window_input_sequences.contains(server_position.acknowledged_input_sequence)) {
                        s1_evidence.movement_boundary_authoritative_updates_complete = false;
                    }
                    s1_evidence.movement_boundary_authoritative_updates.push_back(*update);
                }
                return;
            }
            GameBenchmarkFullRadiusEvidence& full_evidence = *benchmark.full_radius_terrain;
            if (!full_radius_movement_started_at || observed_at < *full_radius_movement_started_at) {
                if (!full_evidence.movement_trajectory_anchor
                    || update->offset_ns > full_evidence.movement_trajectory_anchor->offset_ns) {
                    full_evidence.movement_trajectory_anchor = *update;
                }
                return;
            }
            bool const x30_trajectory = options.workload == GameBenchmarkWorkload::OrdinaryTurningMovement;
            if (x30_trajectory) {
                Clock::time_point const movement_end = *full_radius_movement_started_at
                    + GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
                if (observed_at >= movement_end + shared::TICK
                    || !movement_input_sequences.contains(server_position.acknowledged_input_sequence)) {
                    return;
                }
            } else if (!fullRadiusMovementObservationActive(
                    full_radius_workload,
                    full_radius_movement_started_at,
                    observed_at
                )) {
                return;
            }
            ++full_evidence.movement_authoritative_update_count;
            full_evidence.movement_authoritative_sphere_updates.push_back(*update);
        },
    };
    player.run(core::Address::localhost(server.port()), '@', &client_hooks, started_at);
    client::WorldLoadingStatus const final_loading_status = player.worldLoadingStatus();
    observeControlledStart(final_loading_status);
    observeS1LoadingStatus(final_loading_status, Clock::now());
    std::optional<Clock::time_point> const full_radius_stopped_at = full_radius_workload
        ? std::optional<Clock::time_point>{ full_radius_measurement_end_at.value_or(Clock::now()) }
        : std::nullopt;
    GameBenchmarkFullRadiusPhase const stopped_phase = full_radius_sequence.phase();
    bool const missing_initial_snapshot = full_radius_workload
        && !benchmark.full_radius_terrain->initial_barrier.scan_performed
        && (stopped_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
            || stopped_phase == GameBenchmarkFullRadiusPhase::Failed);
    bool const missing_recovery_snapshot = full_radius_workload
        && !benchmark.full_radius_terrain->recovery_barrier.scan_performed
        && (stopped_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup
            || (stopped_phase == GameBenchmarkFullRadiusPhase::Failed
                && benchmark.full_radius_terrain->initial_barrier_reached));
    if (missing_initial_snapshot || missing_recovery_snapshot) {
        GameBenchmarkFullRadiusEvidence& full_evidence = *benchmark.full_radius_terrain;
        client::PlayerClientBenchmarkFullTerrainInfo const final_info =
            player.benchmarkFullTerrainStreamingInfo(true);
        std::chrono::steady_clock::duration const elapsed = full_radius_sequence.phaseElapsed(Clock::now());
        GameBenchmarkFullTerrainSnapshot const final_snapshot = fullTerrainSnapshot(
            final_info,
            player.benchmarkRuntimeInfo().stone_face_growth,
            player.worldLoadingStatus(),
            stopped_phase,
            elapsed
        );
        if (missing_initial_snapshot) {
            full_evidence.initial_barrier = final_snapshot;
            full_evidence.initial_warmup_elapsed_ms = final_snapshot.phase_elapsed_ms;
        } else if (missing_recovery_snapshot) {
            full_evidence.recovery_barrier = final_snapshot;
            full_evidence.recovery_warmup_elapsed_ms = final_snapshot.phase_elapsed_ms;
        }
    }
    server_run.stopAndJoin();

    if (s1_terrain_workload) {
        server::WorldPreparationSnapshot const final_preparation = world_preparation->latest();
        if (final_preparation.terrain_plan) {
            terrain_backend_receipt = final_preparation.terrain_plan->receipt;
            benchmark.terrain_backend_receipt = terrain_backend_receipt;
        }
    }

    if (permission_error) {
        return std::unexpected("game benchmark permission publication failed: " + *permission_error);
    }

    std::vector<std::chrono::nanoseconds> server_tick_durations;
    server_tick_durations.reserve(raw_server_ticks.size());
    uint64_t server_events_processed = 0U;
    for (ServerTickSample const& tick : raw_server_ticks) {
        bool include_tick = false;
        if (s1_terrain_workload && s1_movement_window.startedAt()) {
            Clock::time_point const movement_start = *s1_movement_window.startedAt();
            include_tick = tick.completed_at >= movement_start
                && tick.completed_at < movement_start + GAME_BENCHMARK_S1_MOVEMENT_WINDOW;
        } else if (first_loop_at) {
            Clock::time_point const tick_window_end = full_radius_workload
                ? full_radius_stopped_at.value_or(Clock::now())
                : *first_loop_at + options.cold_duration + options.warm_duration + options.uncapped_duration;
            include_tick = tick.completed_at >= *first_loop_at && tick.completed_at < tick_window_end;
        }
        if (include_tick) {
            server_tick_durations.push_back(tick.duration);
            server_events_processed += tick.network_events;
        }
    }

    client::RendererRuntimeInfo const runtime = player.benchmarkRuntimeInfo();
    if (!runtime.gpu_name.empty() && runtime.gpu_name != "presentation-context") {
        benchmark.gpu = runtime.gpu_name;
    }
    benchmark.present_mode = presentModeName(runtime.present_mode);
    benchmark.terrain_identity = player.activeTerrainIdentity();
    if (auto const session = player.activeTerrainSession()) {
        benchmark.session_join_epoch = session->join_epoch;
    }
    benchmark.framebuffer_width = runtime.width;
    benchmark.framebuffer_height = runtime.height;
    benchmark.server_events_processed = server_events_processed;
    if (!benchmark.raw_frames.empty()) {
        benchmark.client_message_payload_bytes_sent =
            benchmark.raw_frames.back().client_message_payload_bytes_sent;
        benchmark.client_message_payload_bytes_received =
            benchmark.raw_frames.back().client_message_payload_bytes_received;
    }
    benchmark.warm_present_requests_per_second = static_cast<double>(warm_successful_present_requests)
        / static_cast<double>(options.warm_duration.count());
    benchmark.uncapped_present_requests_per_second = static_cast<double>(uncapped_successful_present_requests)
        / static_cast<double>(options.uncapped_duration.count());
    benchmark.cold_loop_timings = summarizeFrameTimings(cold_durations);
    benchmark.warm_loop_timings = summarizeFrameTimings(warm_durations);
    benchmark.uncapped_loop_timings = summarizeFrameTimings(uncapped_durations);
    benchmark.server_tick_timings = summarizeFrameTimings(server_tick_durations);
    benchmark.permission_publish_timings = summarizeFrameTimings(raw_permission_publish_durations);
    benchmark.permission_publishes = permission_publish_count;
    if (options.workload == GameBenchmarkWorkload::Speed200Movement || full_radius_workload) {
        benchmark.terrain_readiness = terrain_readiness_tracker.evidence();
    }
    if (full_radius_workload && benchmark.full_radius_terrain) {
        GameBenchmarkFullRadiusEvidence& full_evidence = *benchmark.full_radius_terrain;
        bool movement_loop_spheres_complete = false;
        for (GameBenchmarkFrameSample const& frame : benchmark.raw_frames) {
            if (frame.terrain_phase != fullRadiusPhaseName(GameBenchmarkFullRadiusPhase::Movement)) {
                continue;
            }
            movement_loop_spheres_complete = true;
            if (!frame.playable_sphere || !playableSphereSampleReady(*frame.playable_sphere)) {
                movement_loop_spheres_complete = false;
                break;
            }
        }
        bool movement_authoritative_spheres_complete =
            full_evidence.movement_authoritative_update_count != 0U
            && full_evidence.movement_authoritative_update_count
                == full_evidence.movement_authoritative_sphere_updates.size();
        for (GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& update
            : full_evidence.movement_authoritative_sphere_updates) {
            if (!playableSphereSampleReady(update.playable_sphere)
                || !samePlayerPosition(
                    update.authoritative_position,
                    *update.playable_sphere.world_loading_center
                )) {
                movement_authoritative_spheres_complete = false;
                break;
            }
        }
        full_evidence.movement_playable_sphere_complete = movement_loop_spheres_complete
            && movement_authoritative_spheres_complete;
        full_evidence.baseline_loop_attempts = static_cast<uint64_t>(baseline_full_radius_durations.size());
        full_evidence.recovered_loop_attempts = static_cast<uint64_t>(recovered_full_radius_durations.size());
        full_evidence.baseline_failed_present_requests = full_evidence.baseline_loop_attempts
            - full_evidence.baseline_present_requests;
        full_evidence.recovered_failed_present_requests = full_evidence.recovered_loop_attempts
            - full_evidence.recovered_present_requests;
        full_evidence.baseline_loop_timings = summarizeFrameTimings(baseline_full_radius_durations);
        full_evidence.recovered_loop_timings = summarizeFrameTimings(recovered_full_radius_durations);
        full_evidence.baseline_successful_present_request_intervals = summarizeFrameTimings(
            gameBenchmarkSuccessfulPresentIntervals(benchmark.raw_frames, "baseline-stationary")
        );
        full_evidence.recovered_successful_present_request_intervals = summarizeFrameTimings(
            gameBenchmarkSuccessfulPresentIntervals(benchmark.raw_frames, "recovered-stationary")
        );
        full_evidence.baseline_present_requests_per_second =
            full_evidence.baseline_stationary_elapsed_ms != 0U
                ? static_cast<double>(full_evidence.baseline_present_requests) * 1'000.0
                    / static_cast<double>(full_evidence.baseline_stationary_elapsed_ms)
                : 0.0;
        full_evidence.recovered_present_requests_per_second =
            full_evidence.recovered_stationary_elapsed_ms != 0U
                ? static_cast<double>(full_evidence.recovered_present_requests) * 1'000.0
                    / static_cast<double>(full_evidence.recovered_stationary_elapsed_ms)
                : 0.0;
        full_evidence.complete = full_radius_sequence.phase() == GameBenchmarkFullRadiusPhase::Complete
            && full_evidence.initial_barrier_reached
            && full_evidence.recovery_barrier_reached
            && full_evidence.movement_playable_sphere_complete
            && full_evidence.initial_barrier.exact_current_coverage.value_or(false)
            && full_evidence.recovery_barrier.exact_current_coverage.value_or(false)
            && full_evidence.initial_barrier.playable_sphere_ready
            && full_evidence.recovery_barrier.playable_sphere_ready
            && full_evidence.initial_barrier.playable_sphere_radius_blocks
                == shared::MAX_SPHERE_RADIUS_BLOCKS
            && full_evidence.recovery_barrier.playable_sphere_radius_blocks
                == shared::MAX_SPHERE_RADIUS_BLOCKS
            && full_evidence.initial_barrier.playable_sphere_required_chunks != 0U
            && full_evidence.recovery_barrier.playable_sphere_required_chunks != 0U
            && full_evidence.initial_barrier.playable_sphere_ready_chunks
                == full_evidence.initial_barrier.playable_sphere_required_chunks
            && full_evidence.recovery_barrier.playable_sphere_ready_chunks
                == full_evidence.recovery_barrier.playable_sphere_required_chunks
            && benchmark.terrain_readiness
            && benchmark.terrain_readiness->current_near_coverage_complete
            && full_evidence.movement_center_change_count != 0U
            && full_evidence.movement_start_center_valid
            && full_evidence.movement_end_center_valid
            && full_evidence.recovery_barrier.center_valid
            && gameBenchmarkRecoveryIsCloserToMovementEnd(
                shared::HeightTileKey{
                    .x = full_evidence.movement_start_center_x,
                    .y = full_evidence.movement_start_center_y,
                },
                shared::HeightTileKey{
                    .x = full_evidence.movement_end_center_x,
                    .y = full_evidence.movement_end_center_y,
                },
                shared::HeightTileKey{
                    .x = full_evidence.recovery_barrier.center_x,
                    .y = full_evidence.recovery_barrier.center_y,
                }
            )
            && full_evidence.baseline_present_requests != 0U
            && full_evidence.recovered_present_requests != 0U
            && full_evidence.baseline_loop_timings.sample_count != 0U
            && full_evidence.recovered_loop_timings.sample_count != 0U;
    }
    benchmark.raw_server_tick_durations_ns.reserve(server_tick_durations.size());
    for (std::chrono::nanoseconds const duration : server_tick_durations) {
        benchmark.raw_server_tick_durations_ns.push_back(positiveNanoseconds(duration));
    }
    benchmark.raw_permission_publish_durations_ns.reserve(raw_permission_publish_durations.size());
    for (std::chrono::nanoseconds const duration : raw_permission_publish_durations) {
        benchmark.raw_permission_publish_durations_ns.push_back(positiveNanoseconds(duration));
    }
    benchmark.raw_server_stream_samples = std::move(raw_server_stream_samples);
    if (std::optional<uint64_t> const target_frame_duration = platformFrameDurationTarget(
        !options.qualification_proof.empty()
    )) {
        GameBenchmarkFrameBudgetWindow const window = full_radius_workload
            ? fullRadiusFrameBudgetWindow(
                benchmark.raw_frames,
                benchmark.raw_fence_confirmed_frames,
                first_loop_at,
                full_radius_movement_started_at,
                full_radius_movement_window_complete
            )
            : s1_terrain_workload
                ? s1FrameBudgetWindow(
                    first_loop_at,
                    s1_movement_window.startedAt(),
                    s1_movement_window_drained
                )
                : ordinaryFrameBudgetWindow(
                benchmark.raw_frames,
                benchmark.raw_fence_confirmed_frames,
                positiveNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    options.cold_duration + options.warm_duration + options.uncapped_duration
                ))
            );
        benchmark.frame_budget = gameBenchmarkFrameBudget(
            benchmark.raw_frames, benchmark.raw_fence_confirmed_frames, *target_frame_duration, window
        );
    }
    if (s1_terrain_workload) {
        static_cast<void>(s1TerrainPerformanceComplete(benchmark));
    }
    const bool full_radius_workload_complete = !full_radius_workload
        || (benchmark.full_radius_terrain && benchmark.full_radius_terrain->complete
            && benchmark.server_tick_timings.sample_count != 0U
            && benchmark.inputs_sent != 0U
            && benchmark.authoritative_player_updates != 0U
            && server_events_processed != 0U);
    const bool controlled_start_complete = benchmark.controlled_start
        && gameBenchmarkStartActionComplete(*benchmark.controlled_start);
    bool const s1_workload_complete = benchmark.s1_terrain_performance
        && benchmark.s1_terrain_performance->complete
        && benchmark.server_tick_timings.sample_count != 0U
        && benchmark.inputs_sent != 0U
        && benchmark.authoritative_player_updates != 0U
        && server_events_processed != 0U
        && !benchmark.raw_server_stream_samples.empty();
    bool const generic_workload_complete = benchmark.cold_loop_timings.sample_count != 0U
        && benchmark.warm_loop_timings.sample_count != 0U
        && benchmark.uncapped_loop_timings.sample_count != 0U
        && benchmark.server_tick_timings.sample_count != 0U
        && benchmark.inputs_sent != 0U
        && benchmark.authoritative_player_updates != 0U
        && server_events_processed != 0U
        && (options.workload != GameBenchmarkWorkload::WrappedBorder
            || benchmark.observed_wrap_crossings != 0U)
        && (options.workload != GameBenchmarkWorkload::PermissionCollisionChurn
            || (benchmark.observed_capability_transitions != 0U
                && benchmark.permission_publishes >= 2U));
    const bool workload_complete = controlled_start_complete
        && (full_radius_workload ? full_radius_workload_complete
            : s1_terrain_workload ? s1_workload_complete : generic_workload_complete);
    const bool readiness_complete = full_radius_workload
        ? benchmark.terrain_readiness && benchmark.terrain_readiness->current_near_coverage_complete
        : s1_terrain_workload
            ? benchmark.s1_terrain_performance
                && benchmark.s1_terrain_performance->movement_playable_sphere_complete
                && benchmark.s1_terrain_performance->playable_sphere_first_ready_offset_ns
                && !benchmark.s1_terrain_performance->playable_deadline_missed
            : options.workload != GameBenchmarkWorkload::Speed200Movement
                || (benchmark.terrain_readiness && benchmark.terrain_readiness->complete);
#if defined(_WIN32)
    bool const frame_budget_required = options.qualification_proof != "terrain-coverage";
#else
    bool const frame_budget_required = options.qualification_proof == "performance";
#endif
    if (benchmark.frame_budget) {
        benchmark.frame_budget->required_for_benchmark_pass = frame_budget_required;
    }
    const bool frame_budget_complete = !frame_budget_required
        || (benchmark.frame_budget && benchmark.frame_budget->passed);
    uint64_t const loop_poll_count = benchmark.presentation_timing
        ? static_cast<uint64_t>(std::ranges::count_if(
            benchmark.presentation_timing->polls,
            [](GameBenchmarkPresentationPoll const& poll) {
                return poll.scope == GameBenchmarkPresentationPollScope::Loop;
            }
        ))
        : 0U;
    uint64_t const captured_attempt_count = benchmark.successful_present_requests
        + benchmark.failed_present_requests;
    const bool presentation_timing_complete = !capture_presentation_history
        || (benchmark.presentation_timing
            && loop_poll_count == captured_attempt_count
            && gameBenchmarkPresentationTimingComplete(*benchmark.presentation_timing));
    const bool near_terrain_incomplete = full_radius_workload && benchmark.terrain_readiness
        && !benchmark.terrain_readiness->current_near_coverage_complete;
    // RuntimeEvidence embeds the benchmark aggregate, so construct it directly in owning storage too.
    std::unique_ptr<RuntimeEvidence> result_storage{ new RuntimeEvidence{
        .mode = "benchmark-game",
        .scenario_version = {},
        .profile = "flight",
        .scenario_name = std::string{ gameBenchmarkWorkloadName(options.workload) },
        .failure = {},
        .seed = shared::WorldConfiguration::SEED,
        .ticks = static_cast<uint64_t>(server_tick_durations.size()),
        .clients_requested = 1U,
        .clients_accepted = 1U,
        .server_events_processed = server_events_processed,
        .inputs_sent = benchmark.inputs_sent,
        .authoritative_tick_ms = static_cast<uint64_t>(shared::TICK.count()),
        .replay_id = {},
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(effective_deadline),
        .passed = workload_complete && readiness_complete && frame_budget_complete
            && presentation_timing_complete,
        .game_benchmark = std::move(benchmark),
    }};
    RuntimeEvidence& result = *result_storage;
    if (near_terrain_incomplete) {
        result.failure = "full-radius travel left missing meshes in the current near terrain window";
    } else if (s1_terrain_workload && !s1_movement_window.startedAt()) {
        result.failure = "S1 x30 movement window did not start before the playable-sphere deadline";
    } else if (!workload_complete) {
        result.failure = "game benchmark did not complete the scripted full-game workload";
    } else if (!readiness_complete && full_radius_workload) {
        result.failure = "full-radius warmup or post-movement terrain coverage did not satisfy its evidence contract";
    } else if (!readiness_complete) {
        result.failure = s1_terrain_workload
            ? "S1 current 640-block horizontal full-volume terrain did not satisfy its deadline or movement coverage contract"
            : "speed-200 current-near terrain readiness did not satisfy its evidence contract";
    } else if (!frame_budget_complete) {
        result.failure = "game benchmark exceeded the 99% fresh-frame interval budget for the fixed movement window";
    } else if (!presentation_timing_complete) {
        result.failure = "Mac performance proof requires complete driver-reported actual presentation timing";
    }
    return std::move(result);
}

} // namespace acceptance
