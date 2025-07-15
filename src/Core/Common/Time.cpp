#include "Time.hpp"
#include <chrono>

namespace core {
    Time Time::now() {
        return { static_cast<usize>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) };
    }
} // namespace core
