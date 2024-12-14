#include "Time.hpp"
#include <chrono>

core::common::Time core::common::Time::now() {
    return { static_cast<size_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) };
}
