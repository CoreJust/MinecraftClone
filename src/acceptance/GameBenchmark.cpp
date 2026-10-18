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

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/RendererBenchmark.hpp>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
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
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace acceptance {

namespace {

using Clock = std::chrono::steady_clock;

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
    if (full_radius_workload && options.render_distance != shared::HEIGHT_TILE_INTEREST_RADIUS) {
        return std::unexpected("speed-200 full-radius recovery requires render distance 256");
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
    client::PlayerClient player{
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{
            .require_immediate_present_mode = options.require_immediate_present_mode,
            .freeze_camera = options.workload == GameBenchmarkWorkload::DiagnosticStationary
                || full_radius_workload,
            .show_debug_hud = full_radius_workload,
            .enable_mesh_stage_diagnostics = options.diagnostic_full_radius_capture,
        },
    };
    std::cout << "[benchmark-game] player renderer ready; connecting\n" << std::flush;

    GameBenchmarkEvidence benchmark{
        .workload = std::string{ gameBenchmarkWorkloadName(options.workload) },
        .package_id = std::string{ shared::PROJECT_NAME },
        .hardware = hardwareName(),
        .gpu = std::nullopt,
        .present_mode = {},
#if defined(NDEBUG)
        .build_mode = "release",
#else
        .build_mode = "debug",
#endif
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
            }}
            : std::nullopt,
        .raw_frames = {},
        .raw_server_stream_samples = {},
        .raw_server_tick_durations_ns = {},
        .raw_permission_publish_durations_ns = {},
    };
    benchmark.raw_frames.reserve(20'000U);
    std::vector<std::chrono::nanoseconds> cold_durations;
    std::vector<std::chrono::nanoseconds> warm_durations;
    std::vector<std::chrono::nanoseconds> uncapped_durations;
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
    client::GameClientBenchmarkHooks const client_hooks{
        .deadline = started_at + effective_deadline,
        .should_stop = [&] {
            if (!first_loop_at) {
                return false;
            }
            if (full_radius_workload) {
                return full_radius_sequence.phase() == GameBenchmarkFullRadiusPhase::Complete
                    || full_radius_sequence.phase() == GameBenchmarkFullRadiusPhase::Failed;
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
                        << " pending_removals=" << full_terrain_info.pending_removal_count << '\n' << std::flush;
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
                    || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup)) {
                record_raw_frame = !last_full_warmup_raw_at
                    || sample.completed_at - *last_full_warmup_raw_at >= std::chrono::seconds{ 1 };
                if (record_raw_frame) {
                    last_full_warmup_raw_at = sample.completed_at;
                }
            }
            GameBenchmarkFrameSample frame_sample{
                .warm = !full_radius_workload && phase == GameBenchmarkPhase::Warm,
                .uncapped = !full_radius_workload && phase == GameBenchmarkPhase::Uncapped,
                .excluded = !full_radius_workload && phase == GameBenchmarkPhase::Outside,
                .presentation_succeeded = sample.presentation_succeeded,
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
                .diagnostic_terrain_spatial_scan_duration_ns = terrain_spatial_scan_duration_ns,
                .renderer_stone_face_growth = full_radius_workload && record_raw_frame
                    && (full_phase == GameBenchmarkFullRadiusPhase::InitialWarmup
                        || full_phase == GameBenchmarkFullRadiusPhase::RecoveryWarmup)
                    ? std::optional{ runtime.stone_face_growth } : std::nullopt,
                .diagnostic_render_attempt_id = runtime.diagnostic_render_attempt_id,
                .diagnostic_presentation_acquire = gameBenchmarkPresentationAcquireSample(
                    runtime.diagnostic_presentation_acquire
                ),
                .diagnostic_frame_recorded = runtime.diagnostic_frame_recorded,
                .diagnostic_indexed_stone_quads = runtime.diagnostic_indexed_stone_quads,
                .diagnostic_stone_indirect = runtime.diagnostic_stone_indirect,
                .diagnostic_frame_slot = runtime.diagnostic_frame_slot,
                .diagnostic_submitted_stone_quad_count = runtime.diagnostic_submitted_stone_quad_count,
                .diagnostic_stone_draw_count = runtime.diagnostic_stone_draw_count,
                .diagnostic_camera = runtime.diagnostic_camera,
                .diagnostic_cpu_acquire_wait_duration_ns = positiveNanoseconds(runtime.cpu_acquire_wait_duration),
                .diagnostic_cpu_command_record_duration_ns = positiveNanoseconds(runtime.cpu_command_record_duration),
                .diagnostic_cpu_visible_range_collection_duration_ns = positiveNanoseconds(
                    runtime.cpu_visible_range_collection_duration
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
            };
            if (record_raw_frame) {
                benchmark.raw_frames.push_back(std::move(frame_sample));
            }
        },
        .on_authoritative_player = [&](shared::Player const& player) {
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
        ? std::optional<Clock::time_point>{ Clock::now() }
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
        .passed = workload_complete && readiness_complete,
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
    }
    return result;
}

} // namespace acceptance
