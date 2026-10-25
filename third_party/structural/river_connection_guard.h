#ifndef STRUCTURAL_WORLDS_RIVER_CONNECTION_GUARD_H
#define STRUCTURAL_WORLDS_RIVER_CONNECTION_GUARD_H

#include "math3d.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

inline std::atomic<uint64_t> river_fallback_count{0};

inline bool canSmoothRiverConnection(const Segment3D& tributary, const Segment3D& trunk)
{
    const Vec3D a(tributary.b, tributary.a);
    const Vec3D b(trunk.a, trunk.b);
    const double a2 = norm_sq(a);
    const double b2 = norm_sq(b);
    const double cross2 = norm_sq(cross(a,b));
    static constexpr double EPS = std::numeric_limits<double>::epsilon();
    const bool usable = a2 > 0 && b2 > 0 && std::isfinite(a2) && std::isfinite(b2)
        && std::isfinite(cross2) && cross2 > 64 * EPS * EPS * a2 * b2
        && (a.z != 0 || b.z != 0);
    if (!usable) {
        river_fallback_count.fetch_add(1,std::memory_order_relaxed);
    }
    return usable;
}

inline uint64_t riverFallbackCount()
{
    return river_fallback_count.load(std::memory_order_relaxed);
}
#endif
