#include "Engine.hpp"
#include <Core/Macro/LanguageVersion.hpp>
#include <Core/Test/TestMain.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/IO/ArgumentParser.hpp>
#include <Core/IO/Console.hpp>
#include <Core/OS/SignalHandler.hpp>
#include "../Config.hpp"

#if !CPP23
#  error This project requires at least C++ 23.
#endif

namespace engine {
    Engine::Engine(char const* const gameName, core::Version const& gameVersion)
        try : m_renderEngine(gameName, gameVersion, m_camera)
    {
        core::note("Engine successfully initialized");
    } catch (...) {
        core::fatal("Got an exception during engine initialization, shutting down...");
    }

    Engine::~Engine() {
        core::onLoggingEnd();
    }

    void Engine::preinitialization(int argc, char** argv) {
        core::setErrorSignalsHandler();
        core::enableUtf8Cout();
        core::ArgumentParser(argc, argv)
            .onOption("debug", [](std::string_view val) { config::g_isDebugEnabled = (val != "false" && val != "off"); });

        TEST_MAIN();
    }

    void Engine::run() {
        try {
            m_renderEngine.run();
        } catch (...) {
            core::fatal("Got an exception during engine running, shutting down...");
        }
    }
} // namespace engine
