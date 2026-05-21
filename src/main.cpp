#include <core/net/Net.hpp>
#include <core/CrashHandler.hpp>
#include <core/IO/Log.hpp>

#include <server/Server.hpp>
#include <client/Client.hpp>

int main(int argc, char** argv) {
    core::Log::ensureInit();
    core::setCrashHandler();
    core::Net::ensureInit();

    try {
        const bool is_server = (argc >= 2 && std::string_view{ argv[1] } == "--server");
        if (is_server) {
            server::Server server{ };
            server.run();
        } else {
            client::Client client{ };
            client.run();
        }
    } catch (const std::runtime_error& e) {
        MC_CRITICAL("Received uncaught exception: {}", e.what());
    } catch (...) {
        MC_CRITICAL("Received unknown uncaught exception");
    }

    core::AtAppExit::exit();
    return 0;
}
