#include <acceptance/EvidenceJson.hpp>
#include <acceptance/GameBenchmark.hpp>

#include <core/IO/Log.hpp>
#include <core/net/Net.hpp>

#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <string_view>

int main(int const argc, char** const argv)
{
    if (argc != 3) {
        std::cerr << "Expected evidence output path and workload\n";
        return 2;
    }
    acceptance::GameBenchmarkWorkload workload;
    std::string_view const requested{ argv[2] };
    if (requested == "ordinary") {
        workload = acceptance::GameBenchmarkWorkload::OrdinaryMovement;
    } else if (requested == "stationary") {
        workload = acceptance::GameBenchmarkWorkload::DiagnosticStationary;
    } else if (requested == "speed-200") {
        workload = acceptance::GameBenchmarkWorkload::Speed200Movement;
    } else if (requested == "wrapped-border") {
        workload = acceptance::GameBenchmarkWorkload::WrappedBorder;
    } else if (requested == "permission-collision") {
        workload = acceptance::GameBenchmarkWorkload::PermissionCollisionChurn;
    } else {
        std::cerr << "Unknown workload\n";
        return 2;
    }
    core::Log::ensureInit(core::LogSettings{ .initial_level = spdlog::level::info });
    core::Net::ensureInit();
    try {
        acceptance::GameBenchmarkOptions const options{
            .cold_duration = std::chrono::seconds{ 180 },
            .warm_duration = std::chrono::seconds{ 10 },
            .uncapped_duration = std::chrono::seconds{ 5 },
            .deadline = std::chrono::seconds{ 240 },
            .require_immediate_present_mode = true,
            .workload = workload,
        };
        auto const result = acceptance::runGameBenchmark(options);
        if (!result.has_value()) {
            std::cerr << result.error() << '\n';
            return 1;
        }
        std::ofstream output{ argv[1] };
        output << acceptance::evidenceJson(*result);
        output.close();
        if (!output) {
            std::cerr << "Failed to write benchmark evidence\n";
            return 1;
        }
        return result->passed ? 0 : 1;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
