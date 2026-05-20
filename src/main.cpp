#include <core/CrashHandler.hpp>
#include <core/Log.hpp>

int main(int argc, char** argv) {
    core::Log::init();
    core::setCrashHandler();

    try {
        const bool is_server = (argc > 2 && std::string_view{ argv[1] } == "--server");
        if (is_server) {
            // Run server.
        } else {
            // Run client.
        }
    } catch (const std::runtime_error& e) {
        CRITICAL("Received uncaught exception: {}", e.what());
    } catch (...) {
        CRITICAL("Received unknown uncaught exception");
    }

    core::Log::destroy();
    return 0;
}
