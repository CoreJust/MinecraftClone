#include <acceptance/EvidenceJson.hpp>
#include <acceptance/RenderCapture.hpp>
#include <acceptance/RendererBenchmark.hpp>
#include <acceptance/ScenarioRunner.hpp>

#include <client/BotClient.hpp>
#include <client/PlayerClient.hpp>
#include <server/GameServer.hpp>

#include <core/common/CrashHandler.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Net.hpp>

#include <shared/scenario/Scenario.hpp>

#include <filesystem>
#include <condition_variable>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

enum class RuntimeMode {
    Scenario,
    RendererBenchmark,
    RendererCapture,
};

struct RuntimeCommand final {
    RuntimeMode mode;
    std::filesystem::path scenario_path;
    std::filesystem::path image_path;
    std::filesystem::path evidence_path;
    bool require_immediate_present_mode{ false };
};

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
        acceptance::RuntimeEvidence const evidence{
            .mode = m_mode,
            .failure = m_mode + " operation exceeded its watchdog deadline",
            .elapsed = m_deadline,
            .deadline = m_deadline,
            .passed = false,
        };
        static_cast<void>(acceptance::writeEvidenceJson(m_evidence_path, evidence));
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
        return RuntimeCommand{
            .mode = RuntimeMode::Scenario,
            .scenario_path = argv[2],
            .evidence_path = argv[4],
        };
    }
    if (argc == 4 && std::string_view{ argv[1] } == "--benchmark-render"
        && std::string_view{ argv[2] } == "--evidence"
    ) {
        return RuntimeCommand{
            .mode = RuntimeMode::RendererBenchmark,
            .evidence_path = argv[3],
        };
    }
    if (argc == 5 && std::string_view{ argv[1] } == "--benchmark-render"
        && std::string_view{ argv[2] } == "--present-immediate"
        && std::string_view{ argv[3] } == "--evidence"
    ) {
        return RuntimeCommand{
            .mode = RuntimeMode::RendererBenchmark,
            .evidence_path = argv[4],
            .require_immediate_present_mode = true,
        };
    }
    if (argc == 6 && std::string_view{ argv[1] } == "--capture-render"
        && std::string_view{ argv[2] } == "--image"
        && std::string_view{ argv[4] } == "--evidence"
    ) {
        return RuntimeCommand{
            .mode = RuntimeMode::RendererCapture,
            .image_path = argv[3],
            .evidence_path = argv[5],
        };
    }
    return std::unexpected(
        "expected '--scenario <file> --evidence <file>', "
        "'--benchmark-render [--present-immediate] --evidence <file>', "
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
    return acceptance::RuntimeEvidence{
        .mode = std::move(mode),
        .failure = std::move(failure),
        .passed = false,
    };
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
    auto const plan = shared::parseScenario(command.scenario_path.string(), *source, LIMITS);
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

} // namespace

bool recoverFromInputError() {
    if (std::cin.eof()) {
        std::cout << "\nEOF detected\n";
        return false;
    }
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    return true;
}

char readChar(std::string_view const prompt, std::string_view const options) {
    std::cout << prompt << ": ";
    char result = '\0';
    while (true) {
        if (!(std::cin >> result)) {
            if (!recoverFromInputError()) {
                exit(1);
            }
            std::cout << "Expected one of {" << options << "}: ";
            continue;
        }
        if (options.contains(result)) {
            break;
        }
        std::cout << "Expected one of {" << options << "}: ";
    }
    return result;
}

std::optional<core::Address> readAddress() {
    std::cout << "Server address (IP:PORT, default is 127.0.0.1:20040): ";
    std::string line;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::getline(std::cin, line);
    if (line.empty()) {
        return core::Address::localhost(20'040);
    }
    size_t const delim = line.find(':');
    if (delim == std::string::npos) {
        std::cout << "Incorrect address format\n";
        return std::nullopt;
    }
    unsigned long const port_value = std::stoul(line.substr(delim + 1));
    if (port_value > std::numeric_limits<uint16_t>::max()) {
        std::cout << "Port is out of range\n";
        return std::nullopt;
    }
    return core::Address::make(line.substr(0, delim), static_cast<uint16_t>(port_value));
}

int main(int argc, char** argv) {
    core::Log::ensureInit(std::nullopt, spdlog::level::debug);
    core::setCrashHandler();
    core::Net::ensureInit();

    int exit_code = 0;
    try {
        bool const is_server = argc == 2 && std::string_view{ argv[1] } == "--server";
        if (is_server) {
            server::GameServer server{ };
            server.run();
        } else if (argc > 1) {
            auto const command = parseRuntimeCommand(argc, argv);
            if (!command.has_value()) {
                std::cerr << command.error() << '\n';
                exit_code = 1;
            } else if (command->mode == RuntimeMode::Scenario) {
                exit_code = runScenarioCommand(*command);
            } else if (command->mode == RuntimeMode::RendererBenchmark) {
                exit_code = runRendererBenchmarkCommand(*command);
            } else {
                exit_code = runRendererCaptureCommand(*command);
            }
        } else {
            bool const is_real = readChar("Are you a real player? (y/n)", "yn") == 'y';
            char const ch = readChar("Choose your character (@ # $ % &)", "@#$%&");
            auto const address = readAddress();
            if (!address) {
                exit_code = 1;
            } else if (is_real) {
                client::PlayerClient client{ };
                client.run(*address, ch);
            } else {
                client::BotClient client{ };
                client.run(*address, ch);
            }
        }
    } catch (std::runtime_error const& e) {
        CORE_CRITICAL("Received uncaught runtime error: {}", e.what());
        exit_code = 1;
    } catch (std::exception const& e) {
        CORE_CRITICAL("Received uncaught exception: {}", e.what());
        exit_code = 1;
    } catch (...) {
        CORE_CRITICAL("Received unknown uncaught exception");
        exit_code = 1;
    }

    core::AtAppExit::exit();
    return exit_code;
}
