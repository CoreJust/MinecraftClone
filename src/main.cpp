#include <Core/IO/Logger.hpp>
#include <Engine/Engine.hpp>

int main(int argc, char** argv) {
    engine::Engine::preinitialization(argc, argv);
    core::setLogLevel(core::LogLevel::Debug);
    engine::Engine engine("Minecraft Clone");
    engine.run();
    return 0;
}
