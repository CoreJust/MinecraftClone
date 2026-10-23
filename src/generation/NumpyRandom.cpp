#include "NumpyRandom.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace generation::detail
{

namespace
{

constexpr uint32_t SEED_INIT_A = 0x43b0'd7e5U;
constexpr uint32_t SEED_MULT_A = 0x931e'8875U;
constexpr uint32_t SEED_INIT_B = 0x8b51'f9ddU;
constexpr uint32_t SEED_MULT_B = 0x58f3'8dedU;
constexpr uint32_t SEED_MIX_MULT_L = 0xca01'f9ddU;
constexpr uint32_t SEED_MIX_MULT_R = 0x4973'f715U;
constexpr uint64_t PCG_MULTIPLIER_LOW = 4'865'540'595'714'422'341ULL;
constexpr uint64_t PCG_MULTIPLIER_HIGH = 2'549'297'995'355'413'924ULL;
constexpr double TWO_PI = 2.0 * std::numbers::pi;

/*
 * The kernel polynomials and short-range reduction are adapted from fdlibm
 * k_cos.c, k_sin.c, and e_rem_pio2.c.
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this software is freely
 * granted, provided that this notice is preserved.
 */
constexpr double COS_ONE = 1.0;
constexpr double COS_HALF = 0.5;
constexpr double COS_S1 = -1.666'666'666'666'663'243'48e-01;
constexpr double COS_S2 = 8.333'333'333'322'489'461'24e-03;
constexpr double COS_S3 = -1.984'126'982'985'794'931'34e-04;
constexpr double COS_S4 = 2.755'731'370'707'006'767'89e-06;
constexpr double COS_S5 = -2.505'076'025'340'686'341'95e-08;
constexpr double COS_S6 = 1.589'690'995'211'550'102'21e-10;
constexpr double COS_C1 = 4.166'666'666'666'660'190'37e-02;
constexpr double COS_C2 = -1.388'888'888'887'410'957'49e-03;
constexpr double COS_C3 = 2.480'158'728'947'672'941'78e-05;
constexpr double COS_C4 = -2.755'731'435'139'066'330'35e-07;
constexpr double COS_C5 = 2.087'572'321'298'174'827'90e-09;
constexpr double COS_C6 = -1.135'964'755'778'819'482'65e-11;
constexpr double INVERSE_PI_OVER_TWO = 6.366'197'723'675'813'824'33e-01;
constexpr double PI_OVER_TWO_HIGH = 1.570'796'326'734'125'614'17e+00;
constexpr double PI_OVER_TWO_LOW = 6.077'100'506'506'192'249'32e-11;

uint32_t hashMix(uint32_t value, uint32_t& hash_const)
{
    value ^= hash_const;
    hash_const *= SEED_MULT_A;
    value *= hash_const;
    value ^= value >> 16U;
    return value;
}

uint32_t mix(uint32_t x, uint32_t y)
{
    uint32_t result = SEED_MIX_MULT_L * x - SEED_MIX_MULT_R * y;
    result ^= result >> 16U;
    return result;
}

double fdlibmKernelCos(double x, double y)
{
    double const z = x * x;
    double const squared = z * z;
    double const r = z * (COS_C1 + z * (COS_C2 + z * COS_C3)) +
        squared * squared * (COS_C4 + z * (COS_C5 + z * COS_C6));
    double const half_z = 0.5 * z;
    double const w = COS_ONE - half_z;
    return w + (((COS_ONE - w) - half_z) + (z * r - x * y));
}

double fdlibmKernelSin(double x, double y)
{
    double const z = x * x;
    double const w = z * z;
    double const r = COS_S2 + z * (COS_S3 + z * COS_S4) + z * w * (COS_S5 + z * COS_S6);
    double const v = z * x;
    return x - ((z * (COS_HALF * y - v * r) - y) - v * COS_S1);
}

double deterministicCos(double argument)
{
    double rounded_quadrant = argument * INVERSE_PI_OVER_TWO + 0x1.8p52;
    rounded_quadrant -= 0x1.8p52;
    int32_t const quadrant = static_cast<int32_t>(rounded_quadrant);
    double const remainder = argument - rounded_quadrant * PI_OVER_TWO_HIGH;
    double const tail = rounded_quadrant * PI_OVER_TWO_LOW;
    double const reduced_high = remainder - tail;
    double const reduced_low = (remainder - reduced_high) - tail;

    switch (static_cast<uint32_t>(quadrant) & 3U)
    {
        case 0:
        {
            return fdlibmKernelCos(reduced_high, reduced_low);
        }
        case 1:
        {
            return -fdlibmKernelSin(reduced_high, reduced_low);
        }
        case 2:
        {
            return -fdlibmKernelCos(reduced_high, reduced_low);
        }
        default:
        {
            return fdlibmKernelSin(reduced_high, reduced_low);
        }
    }
}

std::array<uint64_t, 4> seedState(uint64_t seed)
{
    std::array<uint32_t, 2> entropy = {
        static_cast<uint32_t>(seed),
        static_cast<uint32_t>(seed >> 32U),
    };
    size_t entropy_size = entropy[1] == 0U ? 1U : 2U;
    std::array<uint32_t, 4> pool = {};

    uint32_t hash_const = SEED_INIT_A;
    for (size_t i = 0; i < pool.size(); ++i)
    {
        uint32_t value = 0U;
        if (i < entropy_size)
        {
            value = entropy[i];
        }
        pool[i] = hashMix(value, hash_const);
    }

    for (size_t i_src = 0; i_src < pool.size(); ++i_src)
    {
        for (size_t i_dst = 0; i_dst < pool.size(); ++i_dst)
        {
            if (i_src != i_dst)
            {
                pool[i_dst] = mix(pool[i_dst], hashMix(pool[i_src], hash_const));
            }
        }
    }

    std::array<uint32_t, 8> words = {};
    hash_const = SEED_INIT_B;
    for (size_t i = 0; i < words.size(); ++i)
    {
        uint32_t value = pool[i % pool.size()] ^ hash_const;
        hash_const *= SEED_MULT_B;
        value *= hash_const;
        value ^= value >> 16U;
        words[i] = value;
    }

    std::array<uint64_t, 4> result = {};
    for (size_t i = 0; i < result.size(); ++i)
    {
        result[i] = static_cast<uint64_t>(words[i * 2U]) |
            (static_cast<uint64_t>(words[i * 2U + 1U]) << 32U);
    }
    return result;
}

struct Product128
{
    uint64_t low;
    uint64_t high;
};

Product128 multiply64(uint64_t x, uint64_t y)
{
    uint64_t const x0 = x & 0xffff'ffffULL;
    uint64_t const x1 = x >> 32U;
    uint64_t const y0 = y & 0xffff'ffffULL;
    uint64_t const y1 = y >> 32U;
    uint64_t const w0 = x0 * y0;
    uint64_t const t = x1 * y0 + (w0 >> 32U);
    uint64_t w1 = t & 0xffff'ffffULL;
    uint64_t const w2 = t >> 32U;
    w1 += x0 * y1;

    return {
        x * y,
        x1 * y1 + w2 + (w1 >> 32U),
    };
}

void advanceState(
    Pcg64State& state,
    Pcg64Increment const& increment
)
{
    uint64_t const old_low = state.low;
    uint64_t const old_high = state.high;
    Product128 const product = multiply64(old_low, PCG_MULTIPLIER_LOW);
    uint64_t const low_without_increment = product.low;

    state.low = product.low + increment.low;
    state.high = product.high + old_high * PCG_MULTIPLIER_LOW +
        old_low * PCG_MULTIPLIER_HIGH + increment.high +
        static_cast<uint64_t>(state.low < low_without_increment);
}

} // namespace

NumpyRandom::NumpyRandom(uint64_t seed)
{
    std::array<uint64_t, 4> const words = seedState(seed);
    uint64_t const init_state_high = words[0];
    uint64_t const init_state_low = words[1];
    uint64_t const init_seq_high = words[2];
    uint64_t const init_seq_low = words[3];

    increment_.low = (init_seq_low << 1U) | 1U;
    increment_.high = (init_seq_high << 1U) | (init_seq_low >> 63U);
    advanceState(state_, increment_);
    state_.low += init_state_low;
    state_.high += init_state_high + static_cast<uint64_t>(state_.low < init_state_low);
    advanceState(state_, increment_);
}

uint64_t NumpyRandom::randomRaw()
{
    advanceState(state_, increment_);
    return std::rotr(state_.low ^ state_.high, static_cast<int>(state_.high >> 58U));
}

uint32_t NumpyRandom::nextUInt32()
{
    if (has_cached_uint32_)
    {
        has_cached_uint32_ = false;
        return cached_uint32_;
    }

    uint64_t const value = randomRaw();
    cached_uint32_ = static_cast<uint32_t>(value >> 32U);
    has_cached_uint32_ = true;
    return static_cast<uint32_t>(value);
}

double NumpyRandom::random()
{
    return static_cast<double>(randomRaw() >> 11U) * (1.0 / 9'007'199'254'740'992.0);
}

double NumpyRandom::uniform(double low, double high)
{
    if (high < low)
    {
        throw std::invalid_argument("NumpyRandom::uniform requires high >= low");
    }
    // Preserve the multiply-add rounding used by NumPy's compiled transform.
    return std::fma(high - low, random(), low);
}

int64_t NumpyRandom::integers(int64_t low, int64_t high)
{
    if (high <= low)
    {
        throw std::invalid_argument("NumpyRandom::integers requires high > low");
    }

    uint64_t const bound = static_cast<uint64_t>(high) - static_cast<uint64_t>(low);
    uint64_t offset = 0U;
    if (bound <= (uint64_t{1} << 32U))
    {
        if (bound == (uint64_t{1} << 32U))
        {
            offset = nextUInt32();
        }
        else
        {
            uint32_t const bound32 = static_cast<uint32_t>(bound);
            uint32_t const threshold = static_cast<uint32_t>(0U - bound32) % bound32;
            for (;;)
            {
                uint64_t const product = static_cast<uint64_t>(nextUInt32()) * bound32;
                uint32_t const product_low = static_cast<uint32_t>(product);
                if (product_low >= threshold)
                {
                    offset = product >> 32U;
                    break;
                }
            }
        }
    }
    else
    {
        uint64_t const threshold = (uint64_t{0} - bound) % bound;
        for (;;)
        {
            Product128 const product = multiply64(randomRaw(), bound);
            if (product.low >= threshold)
            {
                offset = product.high;
                break;
            }
        }
    }

    return std::bit_cast<int64_t>(static_cast<uint64_t>(low) + offset);
}

double NumpyRandom::vonmises(VonMisesParameters const& parameters)
{
    double const mean = parameters.mean;
    double const concentration = parameters.concentration;
    if (std::isnan(concentration))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (concentration < 0.0)
    {
        throw std::invalid_argument("NumpyRandom::vonmises requires nonnegative concentration");
    }
    if (concentration < 1e-8)
    {
        return std::numbers::pi * (2.0 * random() - 1.0);
    }

    double s = 0.0;
    if (concentration < 1e-5)
    {
        s = (1.0 / concentration + concentration);
    }
    else if (concentration <= 1e6)
    {
        double const r = 1.0 + std::sqrt(1.0 + 4.0 * concentration * concentration);
        double const rho = (r - std::sqrt(2.0 * r)) / (2.0 * concentration);
        // NumPy contracts these two multiply-adds; spell them out with FMA while
        // the project disables implicit floating-point contraction.
        s = std::fma(rho, rho, 1.0) / (2.0 * rho);
    }
    else
    {
        throw std::out_of_range("NumpyRandom::vonmises concentration above 1e6 is unsupported");
    }

    double W = 0.0;
    for (;;)
    {
        double const U = random();
        double const Z = deterministicCos(std::numbers::pi * U);
        W = std::fma(s, Z, 1.0) / (s + Z);
        double const Y = concentration * (s - W);
        double const V = random();
        if ((Y * (2.0 - Y) - V >= 0.0) || (std::log(Y / V) + 1.0 - Y >= 0.0))
        {
            break;
        }
    }

    double result = std::acos(W);
    if (random() < 0.5)
    {
        result = -result;
    }
    result += mean;
    bool const negative = result < 0.0;
    double mod = std::fabs(result);
    mod = std::fmod(mod + std::numbers::pi, TWO_PI) - std::numbers::pi;
    if (negative)
    {
        mod *= -1.0;
    }
    return mod;
}

} // namespace generation::detail
