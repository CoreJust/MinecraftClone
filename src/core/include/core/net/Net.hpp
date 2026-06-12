#pragma once

#include <core/common/StaticInitializer.hpp>

#include <atomic>

namespace core {

class NetInitDestroy final {
public:
    static void init();
    static void destroy();
};

using Net = StaticInitializer<NetInitDestroy>;

} // namespace core
