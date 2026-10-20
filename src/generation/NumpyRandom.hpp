#pragma once

#include <cstdint>

namespace generation::detail
{

class NumpyRandom final
{
public:
    explicit NumpyRandom(uint64_t seed);

    [[nodiscard]] uint64_t randomRaw();
    [[nodiscard]] double random();
    [[nodiscard]] double uniform(double low, double high);
    [[nodiscard]] int64_t integers(int64_t low, int64_t high);
    [[nodiscard]] double vonmises(double mean, double concentration);

private:
    [[nodiscard]] uint32_t nextUInt32();

    uint64_t state_low_ = 0U;
    uint64_t state_high_ = 0U;
    uint64_t increment_low_ = 0U;
    uint64_t increment_high_ = 0U;
    uint32_t cached_uint32_ = 0U;
    bool has_cached_uint32_ = false;
};

} // namespace generation::detail
