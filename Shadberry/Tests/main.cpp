#include <Shadberry/Version.hpp>
#include <Shadberry/Core/Console.hpp>

#include <iostream>

int main() {
    engine::setConsoleColor(engine::ConsoleColor::Red);
    std::cout << "EngineTests running\n";
    std::cout << "Engine " << engine::ENGINE_NAME << " v" << engine::ENGINE_VERSION << "\n";
    return 0;
}
