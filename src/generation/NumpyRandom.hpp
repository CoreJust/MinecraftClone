#pragma once

#include <cstdint>

namespace generation::detail
{

struct Pcg64State final
{
    uint64_t low = 0U;
    uint64_t high = 0U;
};

struct Pcg64Increment final
{
    uint64_t low = 0U;
    uint64_t high = 0U;
};

struct VonMisesParameters final
{
    double mean;
    double concentration;
};

class NumpyRandom final
{
public:
    explicit NumpyRandom(uint64_t seed);

    [[nodiscard]] uint64_t randomRaw();
    [[nodiscard]] double random();
    [[nodiscard]] double uniform(double low, double high);
    [[nodiscard]] int64_t integers(int64_t low, int64_t high);
    [[nodiscard]] double vonmises(VonMisesParameters const& parameters);

private:
    [[nodiscard]] uint32_t nextUInt32();

    Pcg64State state_;
    Pcg64Increment increment_;
    uint32_t cached_uint32_ = 0U;
    bool has_cached_uint32_ = false;
};

} // namespace generation::detail
