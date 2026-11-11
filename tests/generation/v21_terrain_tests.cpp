#include "V21Terrain.hpp"

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

// DONT_CHECK INCLUDE_ORDER: the frozen structural header requires <algorithm> first.
#include <algorithm>

#if defined(__clang__)
#pragma clang fp contract(on)
#endif
// DONT_CHECK INCLUDE_ORDER: keep the frozen structural header after <algorithm>.
#include "world_surface.h"
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include <bit>
#include <stop_token>
#include <thread>

namespace
{
using namespace generation::detail;

V9PlatecSourceResult source()
{
    V9PlatecSourceResult result;
    result.height_raw.resize(256U * 256U);
    result.current_plate_id.resize(256U * 256U);
    for (size_t i = 0U; i < result.height_raw.size(); ++i)
    {
        result.height_raw[i] = static_cast<float>((i * 17U) % 97U) / 32.0F;
    }
    return result;
}

} // namespace

TEST(V21TerrainTests, CanonicalFieldsMatchSequentialFullGridOracle)
{
    auto const caller = std::this_thread::get_id();
    uint32_t callbacks = 0U;
    auto result = prepareV21Terrain(42U, source(), {}, [&](V9TerrainStage) {
        EXPECT_EQ(std::this_thread::get_id(), caller);
        ++callbacks;
    });
    SeededWorldSurface const sequential{
        result.native.control_layers, result.native.structural_layers, result.native.dendry_seed,
    };
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            double const expected = 2000.0
                * sequential.terrain().evaluateTerrainUncached(
                    static_cast<double>(x) / 8.0, static_cast<double>(y) / 8.0
                );
            ASSERT_EQ(std::bit_cast<uint64_t>(result.canonical_d3_vertices_m[y * 256U + x]),
                std::bit_cast<uint64_t>(expected)) << "vertex " << x << ',' << y;
        }
    }
    EXPECT_EQ(callbacks, 4U);
}

TEST(V21TerrainTests, CancellationAtCanonicalBoundaryDoesNotPublishCompletion)
{
    std::stop_source cancellation;
    bool complete = false;
    EXPECT_THROW(static_cast<void>(prepareV21Terrain(42U, source(), cancellation.get_token(), [&](V9TerrainStage stage) {
        if (stage == V9TerrainStage::CanonicalVertices)
        {
            cancellation.request_stop();
        }
        complete = complete || stage == V9TerrainStage::Complete;
    })), generation::GenerationCancelled);
    EXPECT_FALSE(complete);
}
