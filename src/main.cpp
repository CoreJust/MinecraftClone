#include <Core/Test/TestMain.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/Common/Defer.hpp>
#include <Core/IO/ArgumentParser.hpp>
#include <Graphics/Window.hpp>
#include "Config.hpp"

int main(int argc, char** argv) {
    core::io::ArgumentParser(argc, argv)
        .onOption("debug", [](auto) { config::g_isDebugEnabled = true; });

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
