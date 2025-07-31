#include "Engine.hpp"
#include <thread>
#include <atomic>
#include <Core/Macro/LanguageVersion.hpp>
#include <Core/Test/TestMain.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/IO/ArgumentParser.hpp>
#include <Core/IO/Console.hpp>
#include <Core/OS/SignalHandler.hpp>
#include <Graphics/RenderEngine.hpp>
#include "../Config.hpp"

#if !CPP23
#  error This project requires at least C++ 23.
#endif

namespace engine {
    Engine::Engine(char const* const gameName, core::Version const& gameVersion)
        try : m_player(core::Vec3d::zero())
            , m_gameName(gameName)
            , m_gameVersion(gameVersion)
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
            std::atomic_bool isRendering = true;
            std::jthread renderThread([&] {
                try {
                    graphics::RenderEngine renderEngine(m_gameName, m_gameVersion, m_camera);
                    renderEngine.run();
                } catch (...) {
                    core::fatal("Caught exception in render thread, shutting down...");
                }
                isRendering = false;
            });
            while (isRendering)
                update();
        } catch (...) {
            core::fatal("Got an exception during engine running, shutting down...");
        }
    }
    
    void Engine::update() {
        try {
            core::Duration delta = m_timer.elapsed();
            m_timer.restart();
            m_player.update(delta);
            m_camera.setPosition(m_player.position);
            m_camera.setRotation(m_player.rotation);
        } catch (...) {
            core::error("Got an exception during engine update");
        }

    }
} // namespace engine
