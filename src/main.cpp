#include <client/BotClient.hpp>
#include <client/PlayerClient.hpp>
#include <server/GameServer.hpp>

#include <core/common/CrashHandler.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Net.hpp>

#include <iostream>
#include <limits>

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
        const bool is_server = (argc >= 2 && std::string_view{ argv[1] } == "--server");
        if (is_server) {
            server::GameServer server{ };
            server.run();
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
