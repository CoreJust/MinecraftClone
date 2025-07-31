#pragma once
#include <Core/Common/Timer.hpp>
#include <Core/Common/Version.hpp>
#include "Camera.hpp"
#include "Entity/Player.hpp"

namespace engine {
    class Engine final {
        Camera m_camera;
        core::Timer m_timer;
        entity::Player m_player;
        char const* const m_gameName;
        core::Version m_gameVersion = { 0, 1, 0, 0 };

    public:
        Engine(char const* const gameName, core::Version const& gameVersion = { 0, 1, 0, 0 });
        ~Engine();

        static void preinitialization(int argc, char** argv);

        void run();

    private:
        void update();
    };
} // namespace engine
