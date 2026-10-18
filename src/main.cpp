#include <client/BotClient.hpp>
#include <client/PlayerClient.hpp>
#include <server/GameServer.hpp>

#include <shared/net/Message.hpp>
#include <shared/scenario/Scenario.hpp>
#include <shared/world/World.hpp>

#include <core/common/AtAppExit.hpp>
#include <core/common/CrashHandler.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Net.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/GameBenchmark.hpp>
#include <acceptance/RenderCapture.hpp>
#include <acceptance/RendererBenchmark.hpp>
#include <acceptance/ScenarioRunner.hpp>

#include <spdlog/common.h>

#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class RuntimeMode : uint8_t {
    Scenario,
    RendererBenchmark,
    GameBenchmark,
    RendererCapture,
};

enum class LaunchMode : uint8_t {
    GraphicalPlayer,
    GraphicalPlayerCapture,
    BotClient,
    Server,
};

struct LaunchCommand final {
    LaunchMode mode;
    core::Address address = core::Address::localhost(20'040);
    std::filesystem::path image_path;
    client::PlayerClientCapturePreset capture_preset = client::PlayerClientCapturePreset::CentralSpike;
    std::optional<shared::PlayerPosition> spawn;
    uint32_t render_distance = shared::HEIGHT_TILE_INTEREST_RADIUS;
};

struct RuntimeCommand final {
    RuntimeMode mode;
    std::filesystem::path scenario_path;
    std::filesystem::path image_path;
    std::filesystem::path evidence_path;
    bool require_immediate_present_mode{ false };
    bool debug_hud_enabled{ false };
    acceptance::GameBenchmarkWorkload game_benchmark_workload =
        acceptance::GameBenchmarkWorkload::OrdinaryMovement;
    uint32_t render_distance = shared::HEIGHT_TILE_INTEREST_RADIUS;
    std::chrono::seconds full_radius_warmup_timeout =
        acceptance::GAME_BENCHMARK_FULL_RADIUS_DEFAULT_WARMUP_TIMEOUT;
    bool diagnostic_full_radius_capture = false;
    bool diagnostic_presentation_stages = false;
    std::optional<uint32_t> diagnostic_indexed_stone_quads;
};

[[nodiscard]]
std::expected<uint32_t, std::string> parseRenderDistance(std::string_view const argument)
{
    uint32_t radius = 0U;
    auto const [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), radius);
    if (error != std::errc{} || end != argument.data() + argument.size()
        || !shared::isValidHeightTileInterestRadius(radius)) {
        return std::unexpected("render distance must be an integer chunk radius from 1 to 256");
    }
    return radius;
}

[[nodiscard]]
std::expected<core::Address, std::string> parseAddress(std::string_view const value)
{
    size_t const delimiter = value.rfind(':');
    if (delimiter == std::string_view::npos || delimiter == 0 || delimiter + 1 == value.size()) {
        return std::unexpected("address must use IP:PORT syntax");
    }

    uint32_t port = 0;
    std::string_view const port_string = value.substr(delimiter + 1);
    auto const [end, error] = std::from_chars(port_string.data(), port_string.data() + port_string.size(), port);
    if (error != std::errc{} || end != port_string.data() + port_string.size() || port == 0
        || port > std::numeric_limits<uint16_t>::max()) {
        return std::unexpected("address contains an invalid port");
    }

    auto address = core::Address::make(
        std::string{ value.substr(0, delimiter) },
        static_cast<uint16_t>(port)
    );
    if (!address) {
        return std::unexpected("address contains an invalid IP address");
    }
    return *address;
}

[[nodiscard]]
std::expected<LaunchCommand, std::string> parseLaunchCommand(int const argc, char** const argv)
{
    if (argc == 1) {
        LaunchCommand command{};
        command.mode = LaunchMode::GraphicalPlayer;
        command.address = core::Address::localhost(20'040);
        return command;
    }

    LaunchMode mode;
    std::string_view const mode_argument{ argv[1] };
    if (mode_argument == "--server") {
        mode = LaunchMode::Server;
    } else if (mode_argument == "--player-client") {
        mode = LaunchMode::GraphicalPlayer;
    } else if (mode_argument == "--player-client-capture") {
        mode = LaunchMode::GraphicalPlayerCapture;
    } else if (mode_argument == "--bot-client") {
        mode = LaunchMode::BotClient;
    } else {
        return std::unexpected("unknown launch option: " + std::string{ mode_argument });
    }

    if (mode == LaunchMode::Server) {
        LaunchCommand command{};
        command.mode = mode;
        command.address = core::Address::localhost(20'040);
        bool port_selected = false;
        bool radius_selected = false;
        for (int index = 2; index < argc; ++index) {
            std::string_view const option{argv[index]};
            if (option == "--port" && !port_selected && index + 1 < argc) {
                auto const address = parseAddress("127.0.0.1:" + std::string{argv[++index]});
                if (!address) {
                    return std::unexpected(address.error());
                }
                command.address = *address;
                port_selected = true;
                continue;
            }
            if (option == "--render-distance" && !radius_selected && index + 1 < argc) {
                auto const radius = parseRenderDistance(argv[++index]);
                if (!radius) {
                    return std::unexpected(radius.error());
                }
                command.render_distance = *radius;
                radius_selected = true;
                continue;
            }
            if (option != "--spawn" || command.spawn || index + 3 >= argc) {
                return std::unexpected(
                    "server launch syntax is '--server [--port PORT] [--spawn X Y Z] [--render-distance CHUNKS]'"
                );
            }
            std::array<int32_t, 3> coordinates{};
            for (uint32_t coordinate = 0U; coordinate < coordinates.size(); ++coordinate) {
                std::string_view const argument{argv[++index]};
                auto const [end, error] = std::from_chars(
                    argument.data(), argument.data() + argument.size(), coordinates[coordinate]
                );
                if (error != std::errc{} || end != argument.data() + argument.size()) {
                    return std::unexpected("server spawn contains an invalid coordinate");
                }
            }
            auto const checked = server::GameServer::validateSpawnPoints({{
                .character = '@', .x = coordinates[0], .y = coordinates[1], .z = coordinates[2],
            }}, shared::WorldMode::Flight);
            if (!checked.has_value()) {
                return std::unexpected(checked.error());
            }
            command.spawn = { .x = coordinates[0], .y = coordinates[1], .z = coordinates[2] };
        }
        return command;
    }
    if (mode == LaunchMode::GraphicalPlayerCapture) {
        if (argc != 8 || std::string_view{argv[2]} != "--address"
            || std::string_view{argv[4]} != "--preset"
            || std::string_view{argv[6]} != "--image") {
            return std::unexpected(
                "capture launch syntax is '--player-client-capture --address IP:PORT "
                "--preset central-spike|trench-first-spike|mountain-climb|first-person-origin --image PATH'"
            );
        }
        auto const address = parseAddress(argv[3]);
        if (!address.has_value()) {
            return std::unexpected(address.error());
        }
        auto const preset = client::parsePlayerClientCapturePreset(argv[5]);
        if (!preset.has_value()) {
            return std::unexpected("capture preset is unknown");
        }
        LaunchCommand command{};
        command.mode = mode;
        command.address = *address;
        command.image_path = argv[7];
        command.capture_preset = *preset;
        return command;
    }
    if (argc == 2) {
        LaunchCommand command{};
        command.mode = mode;
        command.address = core::Address::localhost(20'040);
        return command;
    }
    if (argc != 4 || std::string_view{ argv[2] } != "--address") {
        return std::unexpected("client launch syntax is '<mode> [--address IP:PORT]'");
    }
    auto const address = parseAddress(argv[3]);
    if (!address.has_value()) {
        return std::unexpected(address.error());
    }
    LaunchCommand command{};
    command.mode = mode;
    command.address = *address;
    return command;
}

class RuntimeDeadlineWatchdog final {
public:
    RuntimeDeadlineWatchdog(
        std::filesystem::path evidence_path,
        std::string mode,
        std::chrono::milliseconds const deadline
    )
        : m_evidence_path(std::move(evidence_path))
        , m_mode(std::move(mode))
        , m_deadline(deadline)
        , m_thread([this] { watch(); })
    { }

    ~RuntimeDeadlineWatchdog() { complete(); }

    void complete()
    {
        {
            std::lock_guard const lock{ m_mutex };
            m_completed = true;
        }
        m_condition.notify_one();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
private:
    void watch()
    {
        std::unique_lock lock{ m_mutex };
        if (m_condition.wait_for(lock, m_deadline, [this] { return m_completed; })) {
            return;
        }
        acceptance::RuntimeEvidence evidence{};
        evidence.mode = m_mode;
        evidence.failure = m_mode + " operation exceeded its watchdog deadline";
        evidence.elapsed = m_deadline;
        evidence.deadline = m_deadline;
        auto const written = acceptance::writeEvidenceJson(m_evidence_path, evidence);
        if (!written.has_value()) {
            std::fprintf(
                stderr,
                "cannot write watchdog failure evidence: %s\n",
                written.error().c_str()
            );
        }
        std::_Exit(1);
    }
private:
    std::filesystem::path m_evidence_path;
    std::string m_mode;
    std::chrono::milliseconds m_deadline;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_completed{ false };
    std::thread m_thread;
};

[[nodiscard]]
std::expected<RuntimeCommand, std::string> parseRuntimeCommand(int const argc, char** const argv)
{
    if (argc == 5 && std::string_view{ argv[1] } == "--scenario"
        && std::string_view{ argv[3] } == "--evidence"
    ) {
        RuntimeCommand command{};
        command.mode = RuntimeMode::Scenario;
        command.scenario_path = argv[2];
        command.evidence_path = argv[4];
        return command;
    }
    if (argc >= 4 && argc <= 6 && std::string_view{ argv[1] } == "--benchmark-render"
        && std::string_view{ argv[argc - 2] } == "--evidence"
    ) {
        RuntimeCommand command{};
        command.mode = RuntimeMode::RendererBenchmark;
        command.evidence_path = argv[argc - 1];
        for (int argument_index = 2; argument_index < argc - 2; ++argument_index) {
            std::string_view const argument{ argv[argument_index] };
            if (argument == "--present-immediate" && !command.require_immediate_present_mode) {
                command.require_immediate_present_mode = true;
            } else if (argument == "--hud" && !command.debug_hud_enabled) {
                command.debug_hud_enabled = true;
            } else {
                return std::unexpected("invalid renderer benchmark option: " + std::string{ argument });
            }
        }
        return command;
    }
    if (argc >= 4 && argc <= 15 && std::string_view{ argv[1] } == "--benchmark-game"
        && std::string_view{ argv[argc - 2] } == "--evidence") {
        RuntimeCommand command{};
        command.mode = RuntimeMode::GameBenchmark;
        command.evidence_path = argv[argc - 1];
        bool workload_selected = false;
        bool radius_selected = false;
        bool full_radius_timeout_selected = false;
        bool diagnostic_full_radius_capture_selected = false;
        bool diagnostic_presentation_stages_selected = false;
        bool diagnostic_indexed_stone_quads_selected = false;
        for (int argument_index = 2; argument_index < argc - 2; ++argument_index) {
            std::string_view const argument{ argv[argument_index] };
            if (argument == "--present-immediate" && !command.require_immediate_present_mode) {
                command.require_immediate_present_mode = true;
            } else if (argument == "--render-distance" && !radius_selected && argument_index + 1 < argc - 2) {
                auto const radius = parseRenderDistance(argv[++argument_index]);
                if (!radius) {
                    return std::unexpected(radius.error());
                }
                command.render_distance = *radius;
                radius_selected = true;
            } else if (argument == "--workload" && !workload_selected && argument_index + 1 < argc - 2) {
                std::string_view const name{ argv[++argument_index] };
                if (name == "ordinary") {
                    command.game_benchmark_workload = acceptance::GameBenchmarkWorkload::OrdinaryMovement;
                } else if (name == "speed-200") {
                    command.game_benchmark_workload = acceptance::GameBenchmarkWorkload::Speed200Movement;
                } else if (name == "wrapped-border") {
                    command.game_benchmark_workload = acceptance::GameBenchmarkWorkload::WrappedBorder;
                } else if (name == "permission-collision") {
                    command.game_benchmark_workload = acceptance::GameBenchmarkWorkload::PermissionCollisionChurn;
                } else if (name == "speed-200-full-radius") {
                    command.game_benchmark_workload =
                        acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery;
                } else {
                    return std::unexpected("invalid game benchmark workload: " + std::string{ name });
                }
                workload_selected = true;
            } else if (argument == "--full-radius-warmup-seconds"
                && !full_radius_timeout_selected && argument_index + 1 < argc - 2) {
                int64_t seconds = 0;
                std::string_view const value{ argv[++argument_index] };
                auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (error != std::errc{} || end != value.data() + value.size()) {
                    return std::unexpected("full-radius warmup timeout must be an integer from 1 to 1800 seconds");
                }
                auto const timeout = acceptance::gameBenchmarkFullRadiusWarmupTimeout(seconds);
                if (!timeout) {
                    return std::unexpected(timeout.error());
                }
                command.full_radius_warmup_timeout = *timeout;
                full_radius_timeout_selected = true;
            } else if (argument == "--diagnostic-full-radius-capture"
                && !diagnostic_full_radius_capture_selected) {
                command.diagnostic_full_radius_capture = true;
                diagnostic_full_radius_capture_selected = true;
            } else if (argument == "--diagnostic-presentation-stages"
                && !diagnostic_presentation_stages_selected) {
                command.diagnostic_presentation_stages = true;
                diagnostic_presentation_stages_selected = true;
            } else if (argument == "--diagnostic-indexed-stone-quads"
                && !diagnostic_indexed_stone_quads_selected && argument_index + 1 < argc - 2) {
                uint32_t mode = 0U;
                std::string_view const value{ argv[++argument_index] };
                auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), mode);
                if (error != std::errc{} || end != value.data() + value.size() || mode > 2U) {
                    return std::unexpected("diagnostic indexed stone quad mode must be 0, 1, or 2");
                }
                command.diagnostic_indexed_stone_quads = mode;
                diagnostic_indexed_stone_quads_selected = true;
            } else {
                return std::unexpected("invalid game benchmark option: " + std::string{ argument });
            }
        }

        if (full_radius_timeout_selected
            && command.game_benchmark_workload != acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery) {
            return std::unexpected("--full-radius-warmup-seconds requires --workload speed-200-full-radius");
        }
        if (diagnostic_full_radius_capture_selected
            && command.game_benchmark_workload != acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery) {
            return std::unexpected("--diagnostic-full-radius-capture requires --workload speed-200-full-radius");
        }
        if (diagnostic_presentation_stages_selected
            && command.game_benchmark_workload != acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery) {
            return std::unexpected("--diagnostic-presentation-stages requires --workload speed-200-full-radius");
        }
        if (diagnostic_indexed_stone_quads_selected
            && command.game_benchmark_workload != acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery) {
            return std::unexpected("--diagnostic-indexed-stone-quads requires --workload speed-200-full-radius");
        }
        return command;
    }
    if (argc == 6 && std::string_view{ argv[1] } == "--capture-render"
        && std::string_view{ argv[2] } == "--image"
        && std::string_view{ argv[4] } == "--evidence"
    ) {
        RuntimeCommand command{};
        command.mode = RuntimeMode::RendererCapture;
        command.image_path = argv[3];
        command.evidence_path = argv[5];
        return command;
    }
    return std::unexpected(
        "expected '--scenario <file> --evidence <file>', "
        "'--benchmark-render [--present-immediate] [--hud] --evidence <file>', "
        "'--benchmark-game [--present-immediate] "
        "[--workload ordinary|speed-200|speed-200-full-radius|wrapped-border|permission-collision] "
        "[--render-distance CHUNKS] [--full-radius-warmup-seconds 1..1800] "
        "[--diagnostic-full-radius-capture] [--diagnostic-presentation-stages] "
        "[--diagnostic-indexed-stone-quads 0|1|2] --evidence <file>', "
        "or '--capture-render --image <ppm> --evidence <file>'"
    );
}

[[nodiscard]]
std::expected<std::string, std::string> readScenarioSource(
    std::filesystem::path const& path,
    uint64_t const max_source_bytes
)
{
    std::ifstream input{ path, std::ios::binary };
    if (!input) {
        return std::unexpected("cannot open scenario file: " + path.string());
    }
    if (max_source_bytes == std::numeric_limits<uint64_t>::max()
        || max_source_bytes + 1U > std::string{ }.max_size()
        || max_source_bytes + 1U > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())
    ) {
        return std::unexpected("scenario source byte limit is not supported by this platform");
    }
    size_t const read_capacity = static_cast<size_t>(max_source_bytes + 1U);
    std::string source(read_capacity, '\0');
    input.read(source.data(), static_cast<std::streamsize>(read_capacity));
    size_t const bytes_read = static_cast<size_t>(input.gcount());
    if (bytes_read > max_source_bytes) {
        return std::unexpected(
            "scenario source exceeds the " + std::to_string(max_source_bytes)
            + "-byte input limit before parsing"
        );
    }
    if (input.bad()) {
        return std::unexpected("cannot read scenario file: " + path.string());
    }
    source.resize(bytes_read);
    return source;
}

[[nodiscard]]
acceptance::RuntimeEvidence failedEvidence(std::string mode, std::string failure)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.mode = std::move(mode);
    evidence.failure = std::move(failure);
    return evidence;
}

[[nodiscard]]
int writeRuntimeEvidence(
    std::filesystem::path const& path,
    acceptance::RuntimeEvidence const& evidence
)
{
    auto const written = acceptance::writeEvidenceJson(path, evidence);
    if (!written.has_value()) {
        std::cerr << written.error() << '\n';
        return 1;
    }
    if (!evidence.passed) {
        std::cerr << evidence.failure << '\n';
        return 1;
    }
    return 0;
}

[[nodiscard]]
std::filesystem::path normalizedOutputPath(std::filesystem::path const& output_path)
{
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(output_path, error);
    if (!error) {
        return result;
    }
    error.clear();
    result = std::filesystem::absolute(output_path, error);
    return error ? output_path.lexically_normal() : result.lexically_normal();
}

[[nodiscard]]
int runScenarioCommand(RuntimeCommand const& command)
{
    static constexpr shared::ScenarioLimits LIMITS{
        .max_source_bytes = 65'536,
        .max_statements = 256,
        .max_actors = 4,
        .max_total_ticks = 10'000,
        .max_operations = 512,
        .max_evidence = 128,
    };
    static constexpr std::chrono::seconds DEADLINE{ 30 };
    if (normalizedOutputPath(command.scenario_path) == normalizedOutputPath(command.evidence_path)) {
        std::cerr << "scenario source and evidence paths must differ\n";
        return 1;
    }
    RuntimeDeadlineWatchdog watchdog{ command.evidence_path, "scenario", DEADLINE };
    auto failureEvidence = [](std::string failure) {
        auto evidence = failedEvidence("scenario", std::move(failure));
        evidence.deadline = std::chrono::duration_cast<std::chrono::milliseconds>(DEADLINE);
        return evidence;
    };
    auto const source = readScenarioSource(command.scenario_path, LIMITS.max_source_bytes);
    if (!source.has_value()) {
        return writeRuntimeEvidence(
            command.evidence_path,
            failureEvidence(source.error())
        );
    }
    auto const plan = shared::parseScenarioSource(command.scenario_path.string(), *source, LIMITS);
    if (!plan.has_value()) {
        std::string const failure = plan.error().filename + ":" + std::to_string(plan.error().location.line)
            + ":" + std::to_string(plan.error().location.column) + " "
            + std::string{ shared::scenarioDiagnosticCodeName(plan.error().code) } + " "
            + plan.error().message;
        return writeRuntimeEvidence(command.evidence_path, failureEvidence(failure));
    }
    auto result = acceptance::runScenario(*plan, {
        .deadline = DEADLINE,
        .network_poll_interval = std::chrono::milliseconds{ 1 },
    });
    if (!result.has_value()) {
        auto evidence = failureEvidence(result.error());
        evidence.scenario_version = std::to_string(plan->version());
        evidence.profile = std::string{ shared::scenarioProfileName(plan->profile()) };
        evidence.scenario_name = command.scenario_path.string();
        evidence.seed = plan->seed();
        return writeRuntimeEvidence(command.evidence_path, evidence);
    }
    result->scenario_name = command.scenario_path.string();
    watchdog.complete();
    return writeRuntimeEvidence(command.evidence_path, *result);
}

[[nodiscard]]
int runRendererBenchmarkCommand(RuntimeCommand const& command)
{
    acceptance::RendererBenchmarkOptions const options{
        .require_immediate_present_mode = command.require_immediate_present_mode,
        .debug_hud_enabled = command.debug_hud_enabled,
    };
    RuntimeDeadlineWatchdog watchdog{ command.evidence_path, "benchmark-render", options.deadline };
    acceptance::RuntimeEvidence evidence = acceptance::collectRuntimeEvidence(
        "benchmark-render",
        [&options] { return acceptance::runRendererBenchmark(options); }
    );
    evidence.deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline);
    watchdog.complete();
    return writeRuntimeEvidence(command.evidence_path, evidence);
}

[[nodiscard]]
int runGameBenchmarkCommand(RuntimeCommand const& command)
{
    if (command.diagnostic_indexed_stone_quads.has_value()) {
        std::string const mode = std::to_string(*command.diagnostic_indexed_stone_quads);
#ifdef _WIN32
        bool const mode_selected = ::_putenv_s("MC_DIAGNOSTIC_INDEXED_STONE_QUADS", mode.c_str()) == 0;
#else
        bool const mode_selected = ::setenv("MC_DIAGNOSTIC_INDEXED_STONE_QUADS", mode.c_str(), 1) == 0;
#endif
        if (!mode_selected) {
            return writeRuntimeEvidence(
                command.evidence_path,
                failedEvidence("benchmark-game", "cannot select diagnostic indexed stone quad mode")
            );
        }
    }
    if (command.diagnostic_full_radius_capture) {
#ifdef _WIN32
        bool const timestamps_enabled = ::_putenv_s("MC_DIAGNOSTIC_GPU_TIMESTAMPS", "1") == 0;
#else
        bool const timestamps_enabled = ::setenv("MC_DIAGNOSTIC_GPU_TIMESTAMPS", "1", 1) == 0;
#endif
        if (!timestamps_enabled) {
            return writeRuntimeEvidence(
                command.evidence_path,
                failedEvidence("benchmark-game", "cannot enable diagnostic GPU timestamps")
            );
        }
    }
    if (command.diagnostic_presentation_stages) {
#ifdef _WIN32
        bool const stages_enabled = ::_putenv_s("MC_DIAGNOSTIC_PRESENTATION_STAGES", "1") == 0;
#else
        bool const stages_enabled = ::setenv("MC_DIAGNOSTIC_PRESENTATION_STAGES", "1", 1) == 0;
#endif
        if (!stages_enabled) {
            return writeRuntimeEvidence(
                command.evidence_path,
                failedEvidence("benchmark-game", "cannot enable diagnostic presentation-stage timings")
            );
        }
    }
    acceptance::GameBenchmarkOptions options{
        .require_immediate_present_mode = command.require_immediate_present_mode,
        .workload = command.game_benchmark_workload,
        .render_distance = command.render_distance,
        .full_radius_warmup_timeout = command.full_radius_warmup_timeout,
        .diagnostic_full_radius_capture = command.diagnostic_full_radius_capture,
    };
    options.deadline = acceptance::gameBenchmarkDeadline(options);
    RuntimeDeadlineWatchdog watchdog{ command.evidence_path, "benchmark-game", options.deadline };
    acceptance::RuntimeEvidence evidence = acceptance::collectRuntimeEvidence(
        "benchmark-game",
        [&options] { return acceptance::runGameBenchmark(options); }
    );
    evidence.deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline);
    watchdog.complete();
    return writeRuntimeEvidence(command.evidence_path, evidence);
}

[[nodiscard]]
int runRendererCaptureCommand(RuntimeCommand const& command)
{
    if (normalizedOutputPath(command.image_path) == normalizedOutputPath(command.evidence_path)) {
        return writeRuntimeEvidence(
            command.evidence_path,
            failedEvidence("capture-render", "capture image and evidence paths must differ")
        );
    }
    acceptance::RenderCaptureOptions const options{ };
    RuntimeDeadlineWatchdog watchdog{ command.evidence_path, "capture-render", options.deadline };
    acceptance::RuntimeEvidence evidence = acceptance::collectRuntimeEvidence(
        "capture-render",
        [&command, &options] { return acceptance::captureRendererFrame(command.image_path, options); }
    );
    evidence.deadline = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline);
    watchdog.complete();
    return writeRuntimeEvidence(command.evidence_path, evidence);
}

void reportUncaughtException(char const* category, char const* details = nullptr)
{
    if (core::Log::acquireLogger()) {
        try {
            if (details != nullptr) {
                CORE_CRITICAL("{}: {}", category, details);
            } else {
                CORE_CRITICAL("{}", category);
            }
            return;
        } catch (...) {
            std::fputs("application logger failed while reporting an uncaught exception\n", stderr);
        }
    }

    if (details != nullptr) {
        std::fprintf(stderr, "%s: %s\n", category, details);
    } else {
        std::fprintf(stderr, "%s\n", category);
    }
}

} // namespace

int main(int argc, char** argv) {
    int exit_code = 0;
    try {
#if defined(__APPLE__)
        // Keep driver submit/present encoding off the game thread unless explicitly overridden.
        if (::setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0) != 0) {
            throw std::runtime_error("Cannot select the default MoltenVK queue submission mode");
        }
#endif
        core::LogSettings log_settings{};
        log_settings.initial_level = spdlog::level::debug;
        core::Log::ensureInit(log_settings);
        core::setCrashHandler();
        core::Net::ensureInit();

        bool const is_runtime_command = argc > 1
            && (std::string_view{ argv[1] } == "--scenario"
                || std::string_view{ argv[1] } == "--benchmark-render"
                || std::string_view{ argv[1] } == "--benchmark-game"
                || std::string_view{ argv[1] } == "--capture-render");
        if (is_runtime_command) {
            auto const command = parseRuntimeCommand(argc, argv);
            if (!command.has_value()) {
                std::cerr << command.error() << '\n';
                exit_code = 1;
            } else if (command->mode == RuntimeMode::Scenario) {
                exit_code = runScenarioCommand(*command);
            } else if (command->mode == RuntimeMode::RendererBenchmark) {
                exit_code = runRendererBenchmarkCommand(*command);
            } else if (command->mode == RuntimeMode::GameBenchmark) {
                exit_code = runGameBenchmarkCommand(*command);
            } else {
                exit_code = runRendererCaptureCommand(*command);
            }
        } else {
            auto const command = parseLaunchCommand(argc, argv);
            if (!command.has_value()) {
                std::cerr << command.error() << '\n';
                exit_code = 1;
            } else if (command->mode == LaunchMode::Server) {
                std::vector<server::GameServer::SpawnPoint> spawn_points;
                if (command->spawn.has_value()) {
                    spawn_points.push_back({
                        .character = '@', .x = command->spawn->x,
                        .y = command->spawn->y, .z = command->spawn->z,
                    });
                }
                server::GameServer server{
                    command->address.port(), std::move(spawn_points), shared::WorldMode::Flight,
                    shared::World::canonicalConfiguration(), command->render_distance,
                };
                server.run();
            } else if (command->mode == LaunchMode::BotClient) {
                client::BotClient client{ shared::WorldMode::Flight };
                client.run(command->address, '#');
            } else if (command->mode == LaunchMode::GraphicalPlayerCapture) {
                auto const client = std::make_unique<client::PlayerClient>(
                    shared::WorldMode::Flight,
                    client::PlayerClientCaptureOptions{
                        .image_path = command->image_path,
                        .preset = command->capture_preset,
                    }
                );
                client->run(command->address, '@');
                exit_code = client->captureSucceeded() ? 0 : 1;
            } else {
                auto const client = std::make_unique<client::PlayerClient>(shared::WorldMode::Flight);
                client->run(command->address, '@');
            }
        }
    } catch (std::runtime_error const& e) {
        reportUncaughtException("Received uncaught runtime error", e.what());
        exit_code = 1;
    } catch (std::exception const& e) {
        reportUncaughtException("Received uncaught exception", e.what());
        exit_code = 1;
    } catch (...) {
        reportUncaughtException("Received unknown uncaught exception");
        exit_code = 1;
    }

    try {
        core::AtAppExit::exit();
    } catch (std::exception const& e) {
        reportUncaughtException("Received uncaught app-exit exception", e.what());
        exit_code = 1;
    } catch (...) {
        reportUncaughtException("Received unknown uncaught app-exit exception");
        exit_code = 1;
    }
    return exit_code;
}
