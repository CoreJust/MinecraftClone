#pragma once
#include <Graphics/RenderEngine.hpp>

namespace engine {
    class Engine final {
        Camera m_camera;
        graphics::RenderEngine m_renderEngine;

    public:
        Engine(char const* const gameName, core::Version const& gameVersion = { 0, 1, 0, 0 });
        ~Engine();

        static void preinitialization(int argc, char** argv);

        void run();
    };
} // namespace engine
