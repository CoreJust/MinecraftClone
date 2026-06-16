#include <client/BotClient.hpp>
#include <client/PlayerClient.hpp>
#include <server/GameServer.hpp>

#include <core/common/CrashHandler.hpp>
#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Net.hpp>

#include <iostream>

char readChar(std::string_view const prompt, std::string_view const options) {
    std::cout << prompt << ": ";
    char result;
    std::cin >> result;
    while (!options.contains(result)) {
        std::cout << "Expected one of {" << options << "}: ";
        std::cin >> result;
    }
    return result;
}

core::Address readAddress() {
    std::cout << "Server address (IP:PORT, default is 127.0.0.1:20040): ";
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) {
        return core::Address::localhost(20'040);
    }
    size_t const delim = line.find(':');
    if (delim == std::string::npos) {
        std::cout << "Incorrect address format; Interpreted as default" << std::endl;
        return core::Address::localhost(20'040);
    }
    std::string const ip = line.substr(0, delim);
    uint16_t const port = static_cast<uint16_t>(std::stoul(line.substr(delim + 1)));
    return core::Address::make(ip, port);
}

int main(int argc, char** argv) {
    core::Log::ensureInit(std::nullopt, spdlog::level::debug);
    core::setCrashHandler();
    core::Net::ensureInit();

    try {
        const bool is_server = (argc >= 2 && std::string_view{ argv[1] } == "--server");
        if (is_server) {
            server::GameServer server{ };
            server.run();
        } else {
            bool const is_real = readChar("Are you a real player? (y/n)", "yn") == 'y';
            char const ch = readChar("Choose your character (@ # $ % &)", "@#$%&");
            core::Address address = readAddress();
            if (is_real) {
                client::PlayerClient client{ };
                client.run(address, ch);
            } else {
                client::BotClient client{ };
                client.run(address, ch);
            }
        }
    } catch (const std::runtime_error& e) {
        CORE_CRITICAL("Received uncaught exception: {}", e.what());
    } catch (...) {
        CORE_CRITICAL("Received unknown uncaught exception");
    }

    core::AtAppExit::exit();
    return 0;
}
