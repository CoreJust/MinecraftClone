#include <Core/Test/TestMain.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/Common/Defer.hpp>
#include <Graphics/Window.hpp>

int main() {
	TEST_MAIN();

    core::io::setLogLevel(core::io::LogLevel::Info);
    defer { core::io::onLoggingEnd(); };
    try {
        graphics::Window window("Minecraft Clone", 800, 600);
        while (window.nextFrame());
    } catch (...) {
        core::io::fatal("Got an exception during engine running, shutting down...");
    }
    return 0;
}
