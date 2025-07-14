#include <Core/Test/TestMain.hpp>
#include <Core/Macro/LanguageVersion.hpp>
#include <Core/Common/Defer.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/IO/ArgumentParser.hpp>
#include <Core/IO/Console.hpp>
#include <Core/OS/SignalHandler.hpp>
#include <Graphics/RenderEngine.hpp>
#include "Config.hpp"

#if !CPP23
#  error This project requires at least C++ 23.
#endif

int main(int argc, char** argv) {
    core::os::setErrorSignalHandlers();
    core::io::enableUtf8Cout();
    core::io::ArgumentParser(argc, argv)
        .onOption("debug", [](std::string_view val) { config::g_isDebugEnabled = (val != "false" && val != "off"); });

	TEST_MAIN();

    core::io::setLogLevel(core::io::LogLevel::Trace);
    defer { core::io::onLoggingEnd(); };
    try {
        graphics::RenderEngine engine("Minecraft Clone", { 0, 0, 1, 0 });
        engine.run();
    } catch (...) {
        core::io::fatal("Got an exception during engine running, shutting down...");
    }
    return 0;
}
