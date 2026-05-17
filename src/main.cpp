#include <core/CrashHandler.hpp>
#include <core/Log.hpp>

int main() {
    core::Log::init();
    core::setCrashHandler();

    // Future implementation will be put here.

    core::Log::destroy();
    return 0;
}
