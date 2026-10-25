#include "bounded_cell_cache.h"
#include "V9Terrain.hpp"

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace
{
using namespace generation::detail;

V9TerrainResult flatStructuralInputs()
{
    V9TerrainResult terrain;
    terrain.control_layers.resize(2U * 256U * 256U, .1);
    terrain.structural_layers.resize(3U * 256U * 256U, 0.0);
    std::fill(terrain.structural_layers.begin() + 2U * 256U * 256U,
        terrain.structural_layers.end(), .1);
    terrain.dendry_seed = 42;
    return terrain;
}

TEST(V9TerrainTest, StructuralQueriesCopyInputsAndIdentifyToroidalSeams)
{
    auto terrain = flatStructuralInputs();
    V9StructuralSurface surface(terrain);
    terrain.control_layers.clear();
    terrain.structural_layers.clear();
    for (uint32_t lod = 0U; lod < 4U; ++lod)
    {
        auto const expected = surface.evaluate(.25, 32'768.5, lod);
        EXPECT_EQ(expected, surface.evaluate(65'536.25, 32'768.5, lod));
        EXPECT_EQ(expected, surface.evaluate(-65'535.75, -32'767.5, lod));
        for (auto value : expected) {
            EXPECT_TRUE(std::isfinite(value));
        }
    }
}

TEST(V9TerrainTest, StructuralQueriesIgnoreFreshSurfaceCacheFillOrder)
{
    static constexpr std::array<std::array<double, 2U>, 3U> QUERIES{{
        {12.25, 65.5}, {8'191.75, 16'385.5}, {32'768.25, 49'152.75},
    }};
    auto const terrain = flatStructuralInputs();
    V9StructuralSurface forward{terrain};
    V9StructuralSurface reverse{terrain};
    std::array<std::array<std::array<double, 13U>, 4U>, QUERIES.size()> forward_results{};

    for (size_t query_index = 0U; query_index < QUERIES.size(); ++query_index) {
        for (uint32_t lod = 0U; lod < 4U; ++lod) {
            forward_results[query_index][lod] = forward.evaluate(
                QUERIES[query_index][0], QUERIES[query_index][1], lod);
        }
    }
    for (size_t query_index = QUERIES.size(); query_index > 0U; --query_index) {
        size_t const index = query_index - 1U;
        for (uint32_t lod = 4U; lod > 0U; --lod) {
            uint32_t const level = lod - 1U;
            EXPECT_EQ(forward_results[index][level], reverse.evaluate(
                QUERIES[index][0], QUERIES[index][1], level));
        }
    }
}

TEST(V9TerrainTest, StructuralQueriesRejectInvalidLodAndCoordinates)
{
    V9StructuralSurface surface(flatStructuralInputs());
    EXPECT_THROW((void)surface.evaluate(0., 0., 4U), std::invalid_argument);
    EXPECT_THROW((void)surface.evaluate(std::numeric_limits<double>::infinity(), 0., 0U), std::invalid_argument);
}

TEST(V9TerrainTest, StructuralCacheBudgetFallsBackToDeterministicRecompute)
{
    using Cache = BoundedCellMemo<uint64_t, 32U>;
    CellBudget budget{Cache::minimumPageCacheBytes()};
    Cache cache{budget};
    static constexpr std::array<std::array<uint32_t, 2U>, 3U> QUERIES{{
        {2U, 3U}, {17U, 9U}, {31U, 30U},
    }};
    std::array<uint64_t, QUERIES.size()> expected{};
    size_t computations = 0U;
    for (size_t index = 0U; index < QUERIES.size(); ++index) {
        auto const [x, y] = QUERIES[index];
        expected[index] = *cache.get(x, y, [&] {
            ++computations;
            return uint64_t{x} * 100U + y;
        });
    }
    for (size_t index = QUERIES.size(); index > 0U; --index) {
        auto const [x, y] = QUERIES[index - 1U];
        EXPECT_EQ(expected[index - 1U], *cache.get(x, y, [&] {
            ++computations;
            return uint64_t{x} * 100U + y;
        }));
    }
    EXPECT_EQ(computations, 2U * QUERIES.size());
    EXPECT_EQ(budget.used(), Cache::minimumPageCacheBytes());
}

TEST(V9TerrainTest, StructuralCacheBudgetAllowsConcurrentRecomputation)
{
    using Cache = BoundedCellMemo<uint64_t, 32U>;
    CellBudget budget{Cache::minimumPageCacheBytes()};
    Cache cache{budget};
    static constexpr size_t THREAD_COUNT = 8U;
    static constexpr uint32_t X = 23U;
    static constexpr uint32_t Y = 17U;
    std::array<uint64_t, THREAD_COUNT> results{};
    std::atomic<size_t> computations{};
    std::array<std::thread, THREAD_COUNT> workers;
    for (size_t index = 0U; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            results[index] = *cache.get(X, Y, [&] {
                computations.fetch_add(1U, std::memory_order_relaxed);
                return uint64_t{X} * 100U + Y;
            });
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (auto result : results) {
        EXPECT_EQ(result, uint64_t{X} * 100U + Y);
    }
    EXPECT_EQ(budget.used(), Cache::minimumPageCacheBytes());
    EXPECT_EQ(cache.entries(), 0U);
    EXPECT_LE(computations.load(std::memory_order_relaxed), THREAD_COUNT);
}

TEST(V9TerrainTest, StructuralCacheBudgetDoesNotRetainTransientMisses)
{
    using Cache = BoundedCellMemo<uint64_t, 32U>;
    CellBudget budget{Cache::minimumPageCacheBytes()};
    Cache cache{budget};
    std::vector<std::weak_ptr<const uint64_t>> transient_values;
    transient_values.reserve(4096U);
    for (uint32_t index = 0U; index < 4096U; ++index) {
        uint32_t const x = index % 32U;
        uint32_t const y = (index / 32U) % 32U;
        auto value = cache.get(x, y, [&] { return uint64_t{x} * 100U + y; });
        transient_values.emplace_back(value);
    }
    EXPECT_TRUE(std::all_of(transient_values.begin(), transient_values.end(),
        [](auto const& value) { return value.expired(); }));
    EXPECT_EQ(cache.entries(), 0U);
    EXPECT_EQ(budget.used(), Cache::minimumPageCacheBytes());
}

TEST(V9TerrainTest, NestedCacheMissRetainsTransientChildUntilParentRelease)
{
    struct MemoNode final
    {
        uint32_t value;
        std::shared_ptr<const MemoNode> child;
    };
    using Cache = BoundedCellMemo<MemoNode, 32U>;
    static constexpr uint32_t X = 5U;
    static constexpr uint32_t Y = 11U;
    static constexpr uint32_t CHILD_VALUE = 37U;
    CellBudget budget{Cache::minimumPageCacheBytes() + sizeof(Cache)};
    Cache child_cache{budget};
    Cache parent_cache{budget};
    auto child = child_cache.get(X, Y, [] { return MemoNode{CHILD_VALUE, nullptr}; });
    std::weak_ptr<const MemoNode> weak_child{child};
    auto parent = parent_cache.get(X, Y, [&] { return MemoNode{1U, child}; });
    child.reset();

    ASSERT_NE(parent->child, nullptr);
    EXPECT_EQ(parent->child->value, CHILD_VALUE);
    EXPECT_FALSE(weak_child.expired());
    EXPECT_EQ(child_cache.pages(), 1U);
    EXPECT_EQ(parent_cache.pages(), 0U);
    EXPECT_EQ(child_cache.entries(), 0U);
    EXPECT_EQ(parent_cache.entries(), 0U);

    parent.reset();
    EXPECT_TRUE(weak_child.expired());
    EXPECT_EQ(budget.used(), Cache::minimumPageCacheBytes() + sizeof(Cache));
}

TEST(V9TerrainTest, PreparationRejectsInvalidSourceAndObservesCancellation)
{
    EXPECT_THROW((void)prepareV9Terrain(42U, {}), std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW((void)prepareV9Terrain(42U, {}, stop.get_token()), generation::GenerationCancelled);
}

TEST(V9TerrainTest, PreparationRejectsNonfiniteSourceAndMissingShore)
{
    V9PlatecSourceResult source;
    source.height_raw.resize(256U * 256U, .1F);
    source.current_plate_id.resize(256U * 256U, 0U);
    source.height_raw[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW((void)prepareV9Terrain(42U, source), std::invalid_argument);
    source.height_raw[0] = .1F;
    EXPECT_THROW((void)prepareV9Terrain(42U, source), std::runtime_error);
}

TEST(V9TerrainTest, ProgressCancellationDoesNotPublishCompletion)
{
    std::stop_source stop;
    std::vector<V9TerrainStage> stages;
    EXPECT_THROW((void)prepareV9Terrain(42U, {}, stop.get_token(), [&](auto stage) {
        stages.push_back(stage);
        stop.request_stop();
    }), generation::GenerationCancelled);
    EXPECT_EQ(stages, (std::vector<V9TerrainStage>{V9TerrainStage::SourceMapping}));
}

TEST(V9TerrainTest, CompletedPreparationRetainsSourceMetresAndOrderedProgress)
{
    auto source = generateV9PlatecSource(2U);
    ASSERT_TRUE(source.has_value());
    std::vector<V9TerrainStage> progress;
    auto const terrain = prepareV9Terrain(2U, *source, {}, [&](auto stage) { progress.push_back(stage); });
    EXPECT_EQ(progress, (std::vector<V9TerrainStage>{
        V9TerrainStage::SourceMapping, V9TerrainStage::StructuralPreparation,
        V9TerrainStage::CanonicalVertices, V9TerrainStage::ProvisionalRouting,
        V9TerrainStage::TerrainEvolution, V9TerrainStage::Complete,
    }));
    ASSERT_EQ(terrain.final_bed_vertices_m.size(), 256U * 256U);
    EXPECT_EQ(terrain.control_layers.size(), 2U * 256U * 256U);
    EXPECT_EQ(terrain.structural_layers.size(), 3U * 256U * 256U);
    for (size_t i = 0U; i < source->height_raw.size(); ++i)
    {
        double const raw = source->height_raw[i];
        EXPECT_EQ(terrain.macro_bed_centres_m[i], static_cast<float>(raw >= 1.
            ? (raw - 1.) * 157.576'557'850'187'8 : -600. * std::pow((1. - raw) / .9, 1.5)));
        EXPECT_TRUE(std::isfinite(terrain.final_bed_vertices_m[i]));
    }
}
} // namespace
