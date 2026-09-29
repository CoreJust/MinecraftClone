#include <acceptance/GameBenchmark.hpp>

#include <client/PlayerClient.hpp>
#include <server/GameServer.hpp>

#include <shared/ProjectInfo.hpp>

#include <core/net/Address.hpp>

#include <acceptance/RendererBenchmark.hpp>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
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

} // namespace

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
        || options.deadline <= options.cold_duration + options.warm_duration + options.uncapped_duration) {
        return std::unexpected("game benchmark has invalid duration limits");
    }

    server::GameServer server{ 0U, {}, shared::WorldMode::Flight };
    std::vector<ServerTickSample> raw_server_ticks;
    raw_server_ticks.reserve(512U);
    server::GameServer::BenchmarkHooks const server_hooks{
        .on_tick = [&](std::chrono::nanoseconds const duration, uint64_t const events) {
            raw_server_ticks.push_back(ServerTickSample{
                .completed_at = Clock::now(),
                .duration = duration,
                .network_events = events,
            });
        },
    };
    ServerRunGuard server_run{ server, server_hooks };
    std::cout << "[benchmark-game] loopback server started\n" << std::flush;
    client::PlayerClient player{
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{
            .require_immediate_present_mode = options.require_immediate_present_mode,
        },
    };
    std::cout << "[benchmark-game] player renderer ready; connecting\n" << std::flush;

    GameBenchmarkEvidence benchmark{
        .package_id = std::string{ shared::PROJECT_NAME },
        .hardware = hardwareName(),
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
    };
    benchmark.raw_frames.reserve(20'000U);
    std::vector<std::chrono::nanoseconds> cold_durations;
    std::vector<std::chrono::nanoseconds> warm_durations;
    std::vector<std::chrono::nanoseconds> uncapped_durations;
    auto const started_at = Clock::now();
    std::optional<Clock::time_point> first_loop_at;
    uint8_t reported_phase = 0U;
    uint64_t warm_successful_present_requests = 0U;
    uint64_t uncapped_successful_present_requests = 0U;
    client::GameClientBenchmarkHooks const client_hooks{
        .deadline = started_at + options.deadline,
        .should_stop = [&] {
            return first_loop_at
                && Clock::now() >= *first_loop_at + options.cold_duration
                    + options.warm_duration + options.uncapped_duration;
        },
        .is_uncapped_phase = [&] {
            return first_loop_at
                && Clock::now() >= *first_loop_at + options.cold_duration + options.warm_duration;
        },
        .input_override = [](uint64_t const ordinal) -> std::optional<shared::Direction> {
            uint8_t const x = ordinal % 40U < 20U ? 1U : 255U;
            return shared::Direction{ .x = x, .y = 0U, .z = 0U };
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            if (!first_loop_at) {
                first_loop_at = sample.started_at;
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
                .latest_gpu_frame_duration_ns = runtime.gpu_frame_duration
                    ? std::optional<uint64_t>{ positiveNanoseconds(*runtime.gpu_frame_duration) }
                    : std::nullopt,
            });
        },
        .on_authoritative_player = [&](shared::Player const&) {
            ++benchmark.authoritative_player_updates;
        },
    };
    player.run(core::Address::localhost(server.port()), '@', &client_hooks);
    server_run.stopAndJoin();

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
    benchmark.raw_server_tick_durations_ns.reserve(server_tick_durations.size());
    for (std::chrono::nanoseconds const duration : server_tick_durations) {
        benchmark.raw_server_tick_durations_ns.push_back(positiveNanoseconds(duration));
    }
    if (benchmark.cold_loop_timings.sample_count == 0U
        || benchmark.warm_loop_timings.sample_count == 0U
        || benchmark.uncapped_loop_timings.sample_count == 0U
        || benchmark.server_tick_timings.sample_count == 0U
        || benchmark.inputs_sent == 0U
        || benchmark.authoritative_player_updates == 0U
        || server_events_processed == 0U) {
        return std::unexpected("game benchmark did not complete the scripted full-game workload");
    }
    return RuntimeEvidence{
        .mode = "benchmark-game",
        .profile = "flight",
        .scenario_name = "loopback-flight-v1",
        .seed = shared::WorldConfiguration::SEED,
        .ticks = static_cast<uint64_t>(server_tick_durations.size()),
        .clients_requested = 1U,
        .clients_accepted = 1U,
        .server_events_processed = server_events_processed,
        .inputs_sent = benchmark.inputs_sent,
        .authoritative_tick_ms = static_cast<uint64_t>(shared::TICK.count()),
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at),
        .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline),
        .passed = true,
        .game_benchmark = std::move(benchmark),
    };
}

} // namespace acceptance
