#include <core/common/AtAppExit.hpp>
#include <core/common/CrashHandler.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Net.hpp>

#include <acceptance/ColumnMapReplay.hpp>
#include <acceptance/ReplayInput.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

std::expected<core::Address, std::string> parseAddress(std::string_view const value)
{
    uint64_t const delimiter = static_cast<uint64_t>(value.rfind(':'));
    if (delimiter == static_cast<uint64_t>(std::string_view::npos) || delimiter == 0U
        || delimiter + 1U == value.size()) {
        return std::unexpected("address must use IP:PORT syntax");
    }

    uint32_t port = 0U;
    std::string_view const port_string = value.substr(delimiter + 1U);
    auto const [end, error] = std::from_chars(port_string.data(), port_string.data() + port_string.size(), port);
    if (error != std::errc{} || end != port_string.data() + port_string.size() || port == 0U
        || port > std::numeric_limits<uint16_t>::max()) {
        return std::unexpected("address contains an invalid port");
    }

    auto address = core::Address::make(std::string{value.substr(0U, delimiter)}, static_cast<uint16_t>(port));
    if (!address) {
        return std::unexpected("address contains an invalid IP address");
    }
    return *address;
}

std::expected<acceptance::ColumnMapReplayOptions, std::string> parseOptions(int argc, char** argv)
{
    static constexpr std::string_view USAGE =
        "usage: mc_column_map --address IP:PORT --replay stationary|ordinary-turning "
        "--seconds 1..120 --evidence DIR";
    if (argc != 9 || std::string_view{argv[1]} != "--address"
        || std::string_view{argv[3]} != "--replay" || std::string_view{argv[5]} != "--seconds"
        || std::string_view{argv[7]} != "--evidence") {
        return std::unexpected(std::string{USAGE});
    }

    std::string const address_text{argv[2]};
    auto const address = parseAddress(address_text);
    if (!address) {
        return std::unexpected(address.error());
    }

    acceptance::GameBenchmarkWorkload workload = acceptance::GameBenchmarkWorkload::DiagnosticStationary;
    std::string_view const replay{argv[4]};
    if (replay == "ordinary-turning") {
        workload = acceptance::GameBenchmarkWorkload::OrdinaryTurningMovement;
    } else if (replay != "stationary") {
        return std::unexpected("2D replay must be stationary or ordinary-turning");
    }

    uint32_t seconds = 0U;
    std::string_view const duration{argv[6]};
    auto const [end, error] = std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
    if (error != std::errc{} || end != duration.data() + duration.size() || seconds < 1U || seconds > 120U) {
        return std::unexpected("2D replay seconds must be an integer from 1 to 120");
    }

    return acceptance::ColumnMapReplayOptions{
        .server_address = *address,
        .server_address_text = address_text,
        .duration = std::chrono::seconds{seconds},
        .workload = workload,
        .evidence_directory = argv[8],
    };
}

} // namespace

int main(int argc, char** argv)
{
    int exit_code = 0;
    try {
        core::Log::ensureInit();
        core::setCrashHandler();
        core::Net::ensureInit();

        auto const options = parseOptions(argc, argv);
        if (!options) {
            std::cerr << options.error() << '\n';
            exit_code = 2;
        } else {
            auto const result = acceptance::runColumnMapReplay(*options);
            if (!result) {
                std::cerr << result.error() << '\n';
                exit_code = 1;
            }
        }
    } catch (std::exception const& exception) {
        std::cerr << "column-map client failed: " << exception.what() << '\n';
        exit_code = 1;
    } catch (...) {
        std::cerr << "column-map client failed with an unknown exception\n";
        exit_code = 1;
    }

    try {
        core::AtAppExit::exit();
    } catch (std::exception const& exception) {
        std::cerr << "column-map client app-exit failed: " << exception.what() << '\n';
        exit_code = 1;
    } catch (...) {
        std::cerr << "column-map client app-exit failed with an unknown exception\n";
        exit_code = 1;
    }
    return exit_code;
}
