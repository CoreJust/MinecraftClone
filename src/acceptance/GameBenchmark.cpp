#include <acceptance/GameBenchmark.hpp>

#include <client/GameClient.hpp>
#include <client/PlayerClient.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <server/GameServer.hpp>

#include <shared/ProjectInfo.hpp>
#include <shared/net/Message.hpp>
#include <shared/policy/Policy.hpp>
#include <shared/world/World.hpp>

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
#include <functional>
#include <iostream>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace acceptance {

namespace {

using Clock = std::chrono::steady_clock;

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

} // namespace

void GameBenchmarkTerrainReadinessTracker::observe(
    GameBenchmarkTerrainObservation const& observation,
    std::chrono::steady_clock::time_point const sampled_at
)
{
    if (!observation.center.has_value()) {
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
        || workload == GameBenchmarkWorkload::WrappedBorder;
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
    if (options.cold_duration <= std::chrono::seconds::zero()
        || options.warm_duration <= std::chrono::seconds::zero()
        || options.uncapped_duration <= std::chrono::seconds::zero()
        || options.deadline <= options.cold_duration + options.warm_duration + options.uncapped_duration
            + (options.workload == GameBenchmarkWorkload::Speed200Movement
                ? GAME_BENCHMARK_TERRAIN_READINESS_DEADLINE : std::chrono::milliseconds::zero())) {
        return std::unexpected("game benchmark has invalid duration limits");
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
    std::vector<std::chrono::nanoseconds> raw_permission_publish_durations;
    std::optional<std::string> permission_error;
    std::atomic<int64_t> measurement_end_ns{ 0 };
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
        .on_preview_metrics = {},
    };
    ServerRunGuard server_run{ server, server_hooks };
    std::cout << "[benchmark-game] loopback server started\n" << std::flush;
    client::PlayerClient player{
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{
            .require_immediate_present_mode = options.require_immediate_present_mode,
            .freeze_camera = options.workload == GameBenchmarkWorkload::DiagnosticStationary,
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
        .raw_frames = {},
        .raw_server_tick_durations_ns = {},
        .raw_permission_publish_durations_ns = {},
    };
    benchmark.raw_frames.reserve(20'000U);
    std::vector<std::chrono::nanoseconds> cold_durations;
    std::vector<std::chrono::nanoseconds> warm_durations;
    std::vector<std::chrono::nanoseconds> uncapped_durations;
    GameBenchmarkTerrainReadinessTracker terrain_readiness_tracker;
    uint64_t last_accepted_near_admissions = 0U;
    uint64_t last_accepted_far_admissions = 0U;
    uint64_t last_far_admissions_while_near_pending = 0U;
    auto const started_at = Clock::now();
    std::optional<Clock::time_point> first_loop_at;
    uint8_t reported_phase = 0U;
    uint64_t warm_successful_present_requests = 0U;
    uint64_t uncapped_successful_present_requests = 0U;
    std::optional<shared::Player> previous_authoritative_player;
    client::GameClientBenchmarkHooks const client_hooks{
        .deadline = started_at + options.deadline,
        .should_stop = [&] {
            if (!first_loop_at) {
                return false;
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
            if (!first_loop_at) {
                return false;
            }
            Clock::time_point const uncapped_start = *first_loop_at
                + options.cold_duration + options.warm_duration;
            Clock::time_point const measurement_end = uncapped_start + options.uncapped_duration;
            return Clock::now() >= uncapped_start && Clock::now() < measurement_end;
        },
        .input_override = [&options, &first_loop_at](uint64_t const ordinal)
            -> std::optional<shared::Direction> {
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
                measurement_end_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        (*first_loop_at + options.cold_duration + options.warm_duration
                            + options.uncapped_duration).time_since_epoch()
                    ).count(),
                    std::memory_order_release
                );
                std::cout << "[benchmark-game] connected; cold interval started\n" << std::flush;
            }
            GameBenchmarkPhase const phase = gameBenchmarkPhaseAt(sample.completed_at, *first_loop_at, options);
            if (phase == GameBenchmarkPhase::Warm && reported_phase == 0U) {
                reported_phase = 1U;
                std::cout << "[benchmark-game] warm interval started\n" << std::flush;
            } else if (phase == GameBenchmarkPhase::Uncapped && reported_phase < 2U) {
                reported_phase = 2U;
                std::cout << "[benchmark-game] uncapped interval started\n" << std::flush;
            }
            std::chrono::nanoseconds const duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                sample.completed_at - sample.started_at
            );
            if (phase == GameBenchmarkPhase::Uncapped) {
                if (sample.presentation_succeeded) {
                    uncapped_durations.push_back(duration);
                }
                uncapped_successful_present_requests += sample.presentation_succeeded ? 1U : 0U;
            } else if (phase == GameBenchmarkPhase::Warm) {
                if (sample.presentation_succeeded) {
                    warm_durations.push_back(duration);
                }
                warm_successful_present_requests += sample.presentation_succeeded ? 1U : 0U;
            } else if (phase == GameBenchmarkPhase::Cold) {
                if (sample.presentation_succeeded) {
                    cold_durations.push_back(duration);
                }
            }
            if (phase == GameBenchmarkPhase::Outside) {
                ++benchmark.excluded_present_requests;
            } else {
                benchmark.successful_present_requests += sample.presentation_succeeded ? 1U : 0U;
                benchmark.failed_present_requests += sample.presentation_succeeded ? 0U : 1U;
            }
            benchmark.inputs_sent += sample.input_sent ? 1U : 0U;
            client::RendererRuntimeInfo const runtime = player.benchmarkRuntimeInfo();
            std::optional<GameBenchmarkFrameSample::TerrainNearSample> terrain_near_sample;
            if (options.workload == GameBenchmarkWorkload::Speed200Movement) {
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
            benchmark.raw_frames.push_back(GameBenchmarkFrameSample{
                .warm = phase == GameBenchmarkPhase::Warm,
                .uncapped = phase == GameBenchmarkPhase::Uncapped,
                .excluded = phase == GameBenchmarkPhase::Outside,
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
                .terrain_near = terrain_near_sample,
                .diagnostic_render_attempt_id = runtime.diagnostic_render_attempt_id,
                .diagnostic_frame_recorded = runtime.diagnostic_frame_recorded,
                .diagnostic_indexed_stone_quads = runtime.diagnostic_indexed_stone_quads,
                .diagnostic_stone_indirect = runtime.diagnostic_stone_indirect,
                .diagnostic_frame_slot = runtime.diagnostic_frame_slot,
                .diagnostic_submitted_stone_quad_count = runtime.diagnostic_submitted_stone_quad_count,
                .diagnostic_stone_draw_count = runtime.diagnostic_stone_draw_count,
                .diagnostic_camera = runtime.diagnostic_camera,
                .diagnostic_cpu_acquire_wait_duration_ns = positiveNanoseconds(runtime.cpu_acquire_wait_duration),
                .diagnostic_cpu_command_record_duration_ns = positiveNanoseconds(runtime.cpu_command_record_duration),
                .diagnostic_cpu_complete_present_wait_duration_ns = positiveNanoseconds(
                    runtime.cpu_complete_present_wait_duration
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
            });
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
    server_run.stopAndJoin();

    if (permission_error) {
        return std::unexpected("game benchmark permission publication failed: " + *permission_error);
    }

    std::vector<std::chrono::nanoseconds> server_tick_durations;
    server_tick_durations.reserve(raw_server_ticks.size());
    uint64_t server_events_processed = 0U;
    for (ServerTickSample const& tick : raw_server_ticks) {
        if (first_loop_at && tick.completed_at >= *first_loop_at
            && tick.completed_at < *first_loop_at + options.cold_duration
                + options.warm_duration + options.uncapped_duration) {
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
    if (options.workload == GameBenchmarkWorkload::Speed200Movement) {
        benchmark.terrain_readiness = terrain_readiness_tracker.evidence();
    }
    benchmark.raw_server_tick_durations_ns.reserve(server_tick_durations.size());
    for (std::chrono::nanoseconds const duration : server_tick_durations) {
        benchmark.raw_server_tick_durations_ns.push_back(positiveNanoseconds(duration));
    }
    benchmark.raw_permission_publish_durations_ns.reserve(raw_permission_publish_durations.size());
    for (std::chrono::nanoseconds const duration : raw_permission_publish_durations) {
        benchmark.raw_permission_publish_durations_ns.push_back(positiveNanoseconds(duration));
    }
    const bool workload_complete = benchmark.cold_loop_timings.sample_count != 0U
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
    const bool readiness_complete = options.workload != GameBenchmarkWorkload::Speed200Movement
        || (benchmark.terrain_readiness && benchmark.terrain_readiness->complete);
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
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = workload_complete && readiness_complete,
        .game_benchmark = std::move(benchmark),
    };
    if (!workload_complete) {
        result.failure = "game benchmark did not complete the scripted full-game workload";
    } else if (!readiness_complete) {
        result.failure = "speed-200 current-near terrain readiness did not satisfy its evidence contract";
    }
    return result;
}

} // namespace acceptance
