#include "NumpyRandom.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <numbers>
#include <string>

namespace
{

TEST(NumpyRandomTests, Seed42RawWordsMatchNumPyPcg64)
{
    static constexpr uint64_t SEED = 42U;
    static constexpr std::array<uint64_t, 8> EXPECTED_RAW = {
        0xc621'fbcd'16d9'2688U,
        0x705a'5661'a791'ffc1U,
        0xdbcd'12c2'6eda'1624U,
        0xb286'b60e'1600'888dU,
        0x181c'01b5'3393'81ebU,
        0xf9c2'62ed'86c7'538cU,
        0xc2da'0d2f'bc5a'4471U,
        0xc93b'82a3'b7ac'9740U,
    };

    generation::detail::NumpyRandom random(SEED);

    for (uint64_t expected : EXPECTED_RAW)
    {
        EXPECT_EQ(random.randomRaw(), expected);
    }
}

TEST(NumpyRandomTests, ScalarTransformsMatchOneNumPySeed42Stream)
{
    static constexpr uint64_t SEED = 42U;
    static constexpr std::array<int64_t, 2> EXPECTED_INTEGERS = {
        -2,
        1,
    };
    static constexpr uint64_t EXPECTED_TURN_BITS = 0x4006'0f78'c474'6b77U;
    static constexpr std::array<uint64_t, 4> EXPECTED_FIELD_BITS = {
        0x40eb'79a2'584d'db42U,
        0x40e6'50d6'c1c2'c011U,
        0x40b8'1c01'b533'9380U,
        0x40ef'384c'5db0'd8eaU,
    };
    static constexpr std::array<uint64_t, 4> EXPECTED_RANDOM_BITS = {
        0x3fe8'5b41'a5f7'8b48U,
        0x3fe9'2770'5476'f592U,
        0x3fc0'6607'0b41'b168U,
        0x3fdc'd31f'8a75'be6eU,
    };
    static constexpr std::array<uint64_t, 3> EXPECTED_VONMISES_BITS = {
        0x3ff3'a236'738d'9c34U,
        0xbff1'2f11'e260'a860U,
        0x3ff9'3a44'1dbf'3c6cU,
    };
    static constexpr double VONMISES_MEAN = 0.75;
    static constexpr double VONMISES_CONCENTRATION = 1.8;

    generation::detail::NumpyRandom random(SEED);

    for (int64_t expected : EXPECTED_INTEGERS)
    {
        EXPECT_EQ(random.integers(-2, 3), expected);
    }

    EXPECT_EQ(
        std::bit_cast<uint64_t>(random.uniform(0.0, 2.0 * std::numbers::pi)),
        EXPECTED_TURN_BITS
    );

    for (uint64_t expected : EXPECTED_FIELD_BITS)
    {
        EXPECT_EQ(std::bit_cast<uint64_t>(random.uniform(0.0, 65'536.0)), expected);
    }

    for (uint64_t expected : EXPECTED_RANDOM_BITS)
    {
        EXPECT_EQ(std::bit_cast<uint64_t>(random.random()), expected);
    }

    for (uint64_t expected : EXPECTED_VONMISES_BITS)
    {
        EXPECT_EQ(
            std::bit_cast<uint64_t>(random.vonmises(generation::detail::VonMisesParameters{
                .mean = VONMISES_MEAN,
                .concentration = VONMISES_CONCENTRATION,
            })),
            expected
        );
    }
}

} // namespace
