#include "platec_compat.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace
{

struct WrappedSquareCase final
{
    uint32_t seed;
    int64_t expected_remainder;
};

} // namespace

TEST(PlatecCompatibilityTests, WrappedSignedSquarePreservesSourceRemainder)
{
    static constexpr std::array<WrappedSquareCase, 5U> CASES{{
        {0U, 0},
        {3'037'000'499U, 41},
        {3'037'000'500U, -112},
        {4'294'967'294U, -252},
        {std::numeric_limits<uint32_t>::max(), -255},
    }};

    for (WrappedSquareCase const& test_case : CASES)
    {
        EXPECT_EQ(
            platec_compat::wrappedSignedSquareRemainder256(test_case.seed),
            test_case.expected_remainder
        ) << "seed=" << test_case.seed;
    }
}
