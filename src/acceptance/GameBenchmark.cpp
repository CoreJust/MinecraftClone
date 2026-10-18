#include <acceptance/GameBenchmark.hpp>

#include <client/GameClient.hpp>
#include <client/PlayerClient.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <server/GameServer.hpp>

#include <shared/ProjectInfo.hpp>
#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <core/graphics/vulkan/Vulkan.hpp>
#include <core/net/Address.hpp>
#include <core/net/Event.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/RendererBenchmark.hpp>

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
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
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
    static constexpr std::string_view ALLOW_SOURCE = R"core(@version("0.1.3.1")
@use minecraft
pub fn policy() {
    policyRule("collision", "minecraft:collision-bypass", 1i64, 1u8)
    policyAssign("all", "collision", 1u8)
}
)core";
    static constexpr std::string_view DENY_SOURCE = R"core(@version("0.1.3.1")
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
            .jit_mode = shared::PolicyJitMode::Disabled,
            .require_interpreter_parity = true,
        }
    );
}

std::string_view fullRadiusPhaseName(GameBenchmarkFullRadiusPhase const phase) noexcept
{
    switch (phase) {
        case GameBenchmarkFullRadiusPhase::InitialWarmup: return "initial-full-radius-warmup";
        case GameBenchmarkFullRadiusPhase::BaselineStationary: return "baseline-stationary";
        case GameBenchmarkFullRadiusPhase::Movement: return "speed-200-movement";
        case GameBenchmarkFullRadiusPhase::RecoveryWarmup: return "full-radius-recovery-warmup";
        case GameBenchmarkFullRadiusPhase::RecoveredStationary: return "recovered-stationary";
        case GameBenchmarkFullRadiusPhase::Complete: return "complete";
        case GameBenchmarkFullRadiusPhase::Failed: return "failed";
    }
    return "unknown";
}

GameBenchmarkFullTerrainSnapshot fullTerrainSnapshot(
    client::PlayerClientBenchmarkFullTerrainInfo const& info,
    client::StoneFaceGrowthDiagnostics const& growth,
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
    return qualification_proof ? 8'333'334U : 2'000'000U;
#elif defined(__APPLE__)
    return qualification_proof ? 8'333'334U : 8'333'333U;
#else
    static_cast<void>(qualification_proof);
    return std::nullopt;
#endif
}

} // namespace

GameBenchmarkFullRadiusSequence::GameBenchmarkFullRadiusSequence(
    std::chrono::seconds const warmup_timeout
) noexcept
    : m_warmup_timeout(warmup_timeout)
{ }

void GameBenchmarkFullRadiusSequence::start(Clock::time_point const started_at) noexcept
{
    m_started = true;
    m_phase_started_at = started_at;
}

void GameBenchmarkFullRadiusSequence::observe(
    Clock::time_point const sampled_at,
    std::optional<bool> const exact_current_coverage
) noexcept
{
    if (!m_started) {
        start(sampled_at);
        return;
    }
    if (m_phase == GameBenchmarkFullRadiusPhase::Complete || m_phase == GameBenchmarkFullRadiusPhase::Failed) {
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
            if (exact_current_coverage.value_or(false)) {
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

std::chrono::steady_clock::duration GameBenchmarkFullRadiusSequence::phaseElapsed(
    Clock::time_point const sampled_at
) const noexcept
{
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
    uint64_t const target_frame_duration_ns,
    uint32_t const required_in_budget_time_permille
) noexcept
{
    GameBenchmarkFrameBudgetEvidence evidence{
        .target_frame_duration_ns = target_frame_duration_ns,
        .required_in_budget_time_permille = required_in_budget_time_permille,
    };
    if (target_frame_duration_ns == 0U || required_in_budget_time_permille > 1'000U) {
        return evidence;
    }

    for (GameBenchmarkFrameSample const& sample : frames) {
        if (sample.excluded) {
            continue;
        }
        if (sample.loop_duration_ns > std::numeric_limits<uint64_t>::max() - evidence.measured_time_ns) {
            return evidence;
        }
        evidence.measured_time_ns += sample.loop_duration_ns;
        ++evidence.sample_count;
        if (sample.loop_duration_ns <= target_frame_duration_ns) {
            evidence.in_budget_time_ns += sample.loop_duration_ns;
        }
    }

    if (evidence.measured_time_ns == 0U || evidence.sample_count == 0U) {
        return evidence;
    }

    uint32_t const over_budget_permille = 1'000U - required_in_budget_time_permille;
    uint64_t const allowed_over_budget_time = (evidence.measured_time_ns / 1'000U) * over_budget_permille
        + ((evidence.measured_time_ns % 1'000U) * over_budget_permille) / 1'000U;
    uint64_t const measured_over_budget_time = evidence.measured_time_ns - evidence.in_budget_time_ns;
    evidence.passed = measured_over_budget_time <= allowed_over_budget_time;
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
    uint64_t previous_loop_ordinal = 0U;
    for (GameBenchmarkSuccessfulPresentationRequest const& request : evidence.successful_requests) {
        uint64_t const generation = request.request.swapchain_generation;
        uint32_t const present_id = request.request.application_present_id;
        if (present_id == 0U || request.loop_ordinal <= previous_loop_ordinal
            || (previous_generation && generation < *previous_generation)
            || (request.phase != "initial-full-radius-warmup"
                && request.phase != "baseline-stationary"
                && request.phase != "speed-200-movement"
                && request.phase != "full-radius-recovery-warmup"
                && request.phase != "recovered-stationary")) {
            return false;
        }

        if (!measurement_generation) {
            if (request.phase == "baseline-stationary") {
                if (previous_generation && generation != *previous_generation) {
                    return false;
                }
                measurement_generation = generation;
            } else if (request.phase != "initial-full-radius-warmup") {
                return false;
            }
        } else if (generation != *measurement_generation
            || request.phase == "initial-full-radius-warmup") {
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
        || evidence.capture_endpoint->loop_ordinal != last_request.loop_ordinal) {
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
    if (returned_count != evidence.records.size()) {
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

[[nodiscard]]
shared::Direction gameBenchmarkDirection(
    GameBenchmarkWorkload const workload,
    uint64_t const ordinal
) noexcept
{
    if (workload == GameBenchmarkWorkload::DiagnosticStationary) {
        return {};
    }
    bool const accelerated_full_axis = workload == GameBenchmarkWorkload::Speed200Movement
        || workload == GameBenchmarkWorkload::WrappedBorder
        || workload == GameBenchmarkWorkload::Speed200FullRadiusRecovery;
    uint8_t const x = accelerated_full_axis ? 127U
        : (ordinal % 40U < 20U ? 1U : static_cast<uint8_t>(-1));
    uint16_t const speedup = accelerated_full_axis ? 200U : 5U;
    return shared::Direction{
        .x = x,
        .y = 0U,
        .accelerated = accelerated_full_axis,
        .speedup = speedup,
        .cycle_movement_capabilities = false,
    };
}

std::string_view gameBenchmarkWorkloadName(GameBenchmarkWorkload const workload) noexcept
{
    switch (workload) {
        case GameBenchmarkWorkload::OrdinaryMovement: return "ordinary-movement-v1";
        case GameBenchmarkWorkload::Speed200Movement: return "speed-200-movement-v2";
        case GameBenchmarkWorkload::WrappedBorder: return "wrapped-border-v1";
        case GameBenchmarkWorkload::PermissionCollisionChurn: return "permission-collision-churn-v1";
        case GameBenchmarkWorkload::DiagnosticStationary: return "diagnostic-stationary-v1";
        case GameBenchmarkWorkload::Speed200FullRadiusRecovery: return "speed-200-full-radius-recovery-v1";
    }
    return "unknown";
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
    bool const full_radius_workload = options.workload == GameBenchmarkWorkload::Speed200FullRadiusRecovery;
    bool capture_presentation_history = options.qualification_proof == "performance";
#if !defined(__APPLE__)
    capture_presentation_history = false;
#endif
    if (!options.qualification_proof.empty()) {
        uint32_t const required_radius = options.qualification_proof == "terrain-coverage" ? 256U : 128U;
        if ((options.qualification_proof != "terrain-coverage" && options.qualification_proof != "performance")
            || !full_radius_workload || options.render_distance != required_radius
            || options.full_radius_warmup_timeout != GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT
            || gameBenchmarkDiagnosticOverridesPresent(options.diagnostic_full_radius_capture)) {
            return std::unexpected("gameplay proof requires its fixed radius, production settings and 180-second warmup");
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
    std::vector<server::GameServer::SpawnPoint> const spawn_points =
        options.workload == GameBenchmarkWorkload::WrappedBorder
            ? std::vector<server::GameServer::SpawnPoint>{
                { .character = '@', .x = shared::World::FLIGHT_MAX_CELL - 2,
                    .y = shared::World::FLIGHT_SPAWN.y, .z = shared::World::FLIGHT_SPAWN.z },
            }
            : std::vector<server::GameServer::SpawnPoint>{};
    server::GameServer server{
        0U, spawn_points, shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(), options.render_distance,
    };
    std::vector<ServerTickSample> raw_server_ticks;
    raw_server_ticks.reserve(512U);
    std::vector<GameBenchmarkServerStreamSample> raw_server_stream_samples;
    if (options.diagnostic_full_radius_capture) {
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
        .on_preview_metrics = options.diagnostic_full_radius_capture
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
                    });
                },
            }
            : std::function<void(core::ClientId, server::GameServer::PreviewStreamMetrics const&)>{},
        .before_height_tile_completion = {},
    };
    ServerRunGuard server_run{ server, server_hooks };
    std::cout << "[benchmark-game] loopback server started\n" << std::flush;
    auto const player_storage = std::make_unique<client::PlayerClient>(
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{
            .require_immediate_present_mode = options.require_immediate_present_mode,
            .freeze_camera = options.workload == GameBenchmarkWorkload::DiagnosticStationary
                || full_radius_workload,
            .show_debug_hud = full_radius_workload,
            .enable_mesh_stage_diagnostics = options.diagnostic_full_radius_capture,
            .enable_presentation_history = capture_presentation_history,
        }
    );
    client::PlayerClient& player = *player_storage;
    std::cout << "[benchmark-game] player renderer ready; connecting\n" << std::flush;

    GameBenchmarkEvidence benchmark{
        .workload = std::string{ gameBenchmarkWorkloadName(options.workload) },
        .qualification_proof = options.qualification_proof,
        .package_id = std::string{ shared::PROJECT_NAME },
        .hardware = hardwareName(),
        .gpu = std::nullopt,
        .present_mode = {},
#if defined(NDEBUG)
        .build_mode = "release",
#else
        .build_mode = "debug",
#endif
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
            }}
            : std::nullopt,
        .presentation_timing = capture_presentation_history
            ? std::optional<GameBenchmarkPresentationTimingEvidence>{GameBenchmarkPresentationTimingEvidence{}}
            : std::nullopt,
        .raw_frames = {},
        .raw_server_stream_samples = {},
        .raw_server_tick_durations_ns = {},
        .raw_permission_publish_durations_ns = {},
    };
    benchmark.raw_frames.reserve(20'000U);
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
    GameBenchmarkFullRadiusSequence full_radius_sequence{ options.full_radius_warmup_timeout };
    uint64_t last_accepted_near_admissions = 0U;
    uint64_t last_accepted_far_admissions = 0U;
    uint64_t last_far_admissions_while_near_pending = 0U;
    auto const started_at = Clock::now();
    std::optional<Clock::time_point> first_loop_at;
    std::optional<Clock::time_point> last_full_progress_at;
    std::optional<Clock::time_point> last_full_warmup_raw_at;
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
    expected_presentation_requests.reserve(50'000U);
    recorded_presentation_requests.reserve(50'120U);
    drain_presentation_requests.reserve(120U);
    last_present_id_by_generation.reserve(16U);
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
    client::GameClientBenchmarkHooks const client_hooks{
        .deadline = started_at + effective_deadline,
        .should_stop = [&] {
            if (!first_loop_at) {
                return false;
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
            if (full_radius_workload || !first_loop_at) {
                return false;
            }
            Clock::time_point const uncapped_start = *first_loop_at
                + options.cold_duration + options.warm_duration;
            Clock::time_point const measurement_end = uncapped_start + options.uncapped_duration;
            return Clock::now() >= uncapped_start && Clock::now() < measurement_end;
        },
        .input_override = [&options, &first_loop_at, &full_radius_sequence, full_radius_workload](
            uint64_t const ordinal
        )
            -> std::optional<shared::Direction> {
            if (full_radius_workload) {
                return full_radius_sequence.phase() == GameBenchmarkFullRadiusPhase::Movement
                    ? gameBenchmarkDirection(options.workload, ordinal)
                    : shared::Direction{};
            }
            if (options.workload == GameBenchmarkWorkload::Speed200Movement && first_loop_at
                && Clock::now() >= *first_loop_at + options.cold_duration
                    + options.warm_duration + options.uncapped_duration) {
                return shared::Direction{};
            }
            return gameBenchmarkDirection(options.workload, ordinal);
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            uint64_t const current_loop_ordinal = ++loop_ordinal;
            if (benchmark.presentation_timing && benchmark.presentation_timing->capture_endpoint) {
                ++presentation_drain_attempts;
                client::RendererRuntimeInfo const runtime = player.benchmarkRuntimeInfo();
                observePresentationGeneration(runtime, true);
                static_cast<void>(captureSuccessfulRequest(
                    sample,
                    runtime,
                    "recovered-stationary",
                    current_loop_ordinal,
                    true
                ));
                pollPresentationHistory(GameBenchmarkPresentationPollScope::Drain);
                updatePresentationDrainCompletion();
                return;
            }
            if (!first_loop_at) {
                first_loop_at = sample.started_at;
                first_loop_started_at_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        sample.started_at.time_since_epoch()
                    ).count(),
                    std::memory_order_release
                );
                if (full_radius_workload) {
                    full_radius_sequence.start(sample.started_at);
                    last_full_progress_at = sample.started_at;
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
                sample.completed_at - sample.started_at
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
            benchmark.inputs_sent += sample.input_sent ? 1U : 0U;
            client::RendererRuntimeInfo const runtime = player.benchmarkRuntimeInfo();
            std::optional<GameBenchmarkPresentationRequestId> presentation_request;
            if (benchmark.presentation_timing) {
                observePresentationGeneration(
                    runtime,
                    full_radius_workload && full_phase != GameBenchmarkFullRadiusPhase::InitialWarmup
                );
                presentation_request = captureSuccessfulRequest(
                    sample,
                    runtime,
                    fullRadiusPhaseName(full_phase),
                    current_loop_ordinal,
                    false
                );
                pollPresentationHistory(GameBenchmarkPresentationPollScope::Loop);
            }
            client::PlayerClientRenderDiagnostics const client_render = player.benchmarkRenderDiagnostics();
            client::PlayerClientBenchmarkFullTerrainInfo full_terrain_info;
            std::optional<GameBenchmarkFullTerrainSnapshot> full_radius_snapshot;
            if (full_radius_workload) {
                full_terrain_info = player.benchmarkFullTerrainStreamingInfo();
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
                        full_terrain_info, runtime.stone_face_growth, full_phase, elapsed
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
                        full_terrain_info, runtime.stone_face_growth, full_phase, elapsed
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
                full_radius_sequence.observe(observation_at, exact_coverage);
                GameBenchmarkFullRadiusPhase const next_full_phase = full_radius_sequence.phase();
                if (previous_full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                    && next_full_phase != previous_full_phase) {
                    full_evidence.initial_barrier_reached = next_full_phase
                        == GameBenchmarkFullRadiusPhase::BaselineStationary;
                    full_evidence.initial_barrier = full_radius_snapshot.value_or(
                        fullTerrainSnapshot(full_terrain_info, runtime.stone_face_growth, previous_full_phase, elapsed)
                    );
                } else if (previous_full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup
                    && next_full_phase != previous_full_phase) {
                    full_evidence.recovery_barrier_reached = next_full_phase
                        == GameBenchmarkFullRadiusPhase::RecoveredStationary;
                    full_evidence.recovery_barrier = full_radius_snapshot.value_or(
                        fullTerrainSnapshot(full_terrain_info, runtime.stone_face_growth, previous_full_phase, elapsed)
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
                client::PlayerClientBenchmarkTerrainInfo const terrain_info =
                    player.benchmarkTerrainStreamingInfo();
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
            bool record_raw_frame = true;
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
            GameBenchmarkFrameSample frame_sample{
                .loop_ordinal = current_loop_ordinal,
                .warm = !full_radius_workload && phase == GameBenchmarkPhase::Warm,
                .uncapped = !full_radius_workload && phase == GameBenchmarkPhase::Uncapped,
                .excluded = full_radius_workload
                    ? !options.qualification_proof.empty()
                        && (full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                            || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup)
                    : phase == GameBenchmarkPhase::Outside,
                .presentation_succeeded = sample.presentation_succeeded,
                .swapchain_generation = presentation_request
                    ? std::optional<uint64_t>{ presentation_request->swapchain_generation } : std::nullopt,
                .application_present_id = presentation_request
                    ? std::optional<uint32_t>{ presentation_request->application_present_id } : std::nullopt,
                .started_offset_ns = positiveNanoseconds(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(sample.started_at - *first_loop_at)
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
                .terrain_phase = full_radius_workload
                    ? std::string{ fullRadiusPhaseName(full_phase) } : std::string{},
                .terrain_center_valid = full_radius_workload && full_terrain_info.center.has_value(),
                .terrain_center_x = full_radius_workload && full_terrain_info.center
                    ? full_terrain_info.center->x : 0,
                .terrain_center_y = full_radius_workload && full_terrain_info.center
                    ? full_terrain_info.center->y : 0,
                .terrain_near = terrain_near_sample,
                .diagnostic_terrain_radial_bands = terrain_radial_bands,
                .diagnostic_mesh_episodes = mesh_episodes,
                .diagnostic_terrain_spatial_scan_duration_ns = terrain_spatial_scan_duration_ns,
                .renderer_stone_face_growth = full_radius_workload && record_raw_frame
                    && (full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                        || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup)
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
            if (record_raw_frame) {
                benchmark.raw_frames.push_back(std::move(frame_sample));
            }
        },
        .on_authoritative_player = [&](shared::Player const& player) {
            if (full_radius_measurement_end_at && Clock::now() >= *full_radius_measurement_end_at) {
                return;
            }
            ++benchmark.authoritative_player_updates;
            if (previous_authoritative_player) {
                if (previous_authoritative_player->x > shared::World::FLIGHT_MAX_CELL - 256
                    && player.x < 256) {
                    ++benchmark.observed_wrap_crossings;
                }
                if (previous_authoritative_player->movement_capabilities
                    != player.movement_capabilities) {
                    ++benchmark.observed_capability_transitions;
                }
            }
            previous_authoritative_player = player;
        },
    };
    player.run(core::Address::localhost(server.port()), '@', &client_hooks);
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
            final_info, player.benchmarkRuntimeInfo().stone_face_growth, stopped_phase, elapsed
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

    if (permission_error) {
        return std::unexpected("game benchmark permission publication failed: " + *permission_error);
    }

    std::vector<std::chrono::nanoseconds> server_tick_durations;
    server_tick_durations.reserve(raw_server_ticks.size());
    uint64_t server_events_processed = 0U;
    for (ServerTickSample const& tick : raw_server_ticks) {
        Clock::time_point const tick_window_end = full_radius_workload
            ? full_radius_stopped_at.value_or(Clock::now())
            : first_loop_at.value_or(tick.completed_at) + options.cold_duration
                + options.warm_duration + options.uncapped_duration;
        if (first_loop_at && tick.completed_at >= *first_loop_at && tick.completed_at < tick_window_end) {
            server_tick_durations.push_back(tick.duration);
            server_events_processed += tick.network_events;
        }
    }

    client::RendererRuntimeInfo const runtime = player.benchmarkRuntimeInfo();
    if (!runtime.gpu_name.empty() && runtime.gpu_name != "presentation-context") {
        benchmark.gpu = runtime.gpu_name;
    }
    benchmark.present_mode = presentModeName(runtime.present_mode);
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
            && full_evidence.initial_barrier.exact_current_coverage.value_or(false)
            && full_evidence.recovery_barrier.exact_current_coverage.value_or(false)
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
        benchmark.frame_budget = gameBenchmarkFrameBudget(benchmark.raw_frames, *target_frame_duration);
    }
    const bool full_radius_workload_complete = !full_radius_workload
        || (benchmark.full_radius_terrain && benchmark.full_radius_terrain->complete
            && benchmark.server_tick_timings.sample_count != 0U
            && benchmark.inputs_sent != 0U
            && benchmark.authoritative_player_updates != 0U
            && server_events_processed != 0U);
    const bool workload_complete = full_radius_workload_complete && (full_radius_workload
        || (benchmark.cold_loop_timings.sample_count != 0U
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
                && benchmark.permission_publishes >= 2U))));
    const bool readiness_complete = options.workload == GameBenchmarkWorkload::Speed200FullRadiusRecovery
        ? benchmark.terrain_readiness && benchmark.terrain_readiness->current_near_coverage_complete
        : options.workload != GameBenchmarkWorkload::Speed200Movement
            || (benchmark.terrain_readiness && benchmark.terrain_readiness->complete);
#if defined(_WIN32)
    bool const frame_budget_required = options.qualification_proof != "terrain-coverage";
#else
    bool const frame_budget_required = false;
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
    RuntimeEvidence result{
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
    };
    if (near_terrain_incomplete) {
        result.failure = "full-radius travel left missing meshes in the current near terrain window";
    } else if (!workload_complete) {
        result.failure = "game benchmark did not complete the scripted full-game workload";
    } else if (!readiness_complete && full_radius_workload) {
        result.failure = "full-radius warmup or post-movement terrain coverage did not satisfy its evidence contract";
    } else if (!readiness_complete) {
        result.failure = "speed-200 current-near terrain readiness did not satisfy its evidence contract";
    } else if (!frame_budget_complete) {
        result.failure = "game benchmark exceeded the platform frame-time budget for more than 0.2% of measured time";
    } else if (!presentation_timing_complete) {
        result.failure = "Mac performance proof requires complete driver-reported actual presentation timing";
    }
    return result;
}

} // namespace acceptance
