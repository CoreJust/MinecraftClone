#include <core/net/Net.hpp>

#include <core/Log.hpp>

#include <enet/enet.h>

namespace core {

void NetInitDestroy::init() {
    if (enet_initialize() != 0) {
        CRITICAL("Failed to initialize network; exiting");
        exit(1);
    }
}

void NetInitDestroy::destroy() {
    enet_deinitialize();
}

} // namespace core
