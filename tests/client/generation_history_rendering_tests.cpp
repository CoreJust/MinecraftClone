#include <client/GenerationMapPresentation.hpp>
#include <client/S2MapPresentation.hpp>

#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>
#include <generation/V10Provider.hpp>
#include <generation/V9Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <variant>

namespace
{
void writeImage(std::filesystem::path const& file, client::GenerationMapImage const& image)
{
    std::ofstream out(file,std::ios::binary);out<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(auto const& pixel:image.rgba)for(size_t c=0U;c<3U;++c)out.put(static_cast<char>(pixel[c]));
}
std::shared_ptr<generation::GenerationSnapshot> livePlatecSnapshot()
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V21Platec;
    snapshot->resolution = 256U;
    auto& fields = snapshot->v21.emplace();
    fields.source_elevation_m.resize(256U * 256U);
    fields.plate_ids.assign(256U * 256U, 1U);
    fields.plate_directions = {{1U, 1.0F, .25F}};
    for (uint32_t y = 0U; y < 256U; ++y) for (uint32_t x = 0U; x < 256U; ++x)
        fields.source_elevation_m[y * 256U + x] = x > 70U && x < 180U && y > 50U && y < 210U ? 1200.0F : -800.0F;
    return snapshot;
}

TEST(GenerationHistoryRenderingTest, DistinctV10LivePlatecStartsAndAdoptsCanonicalMetresWithoutRestart)
{
    auto snapshot=std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage=generation::GenerationStage::V10Platec;
    snapshot->resolution=256U;
    auto& source=snapshot->v10.emplace();
    source.height_raw.resize(256U*256U);
    source.plate_ids.assign(256U*256U,1U);
    source.plate_directions={{1U,1.0F,.25F}};
    for(uint32_t y=0U;y<256U;++y) for(uint32_t x=0U;x<256U;++x)
        source.height_raw[y*256U+x]=1.0F+static_cast<float>(x)/256.0F;
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    client::GenerationMapAnimator animator;
    auto first=animator.updateNative(snapshot,nullptr,nullptr,{});
    ASSERT_NE(first.image,nullptr);
    auto cooling=animator.updateNative(snapshot,nullptr,nullptr,{.visible_elapsed=std::chrono::milliseconds{5000}});
    ASSERT_NE(cooling.image,nullptr);
    EXPECT_NE(first.image->rgba,cooling.image->rgba);
    auto canonical=std::make_shared<generation::GenerationSnapshot>(*snapshot);
    canonical->stage=generation::GenerationStage::V10Terrain;
    canonical->v10->source_elevation_m.assign(256U*256U,200.0);
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*canonical));
    auto adopted=animator.updateNative(canonical,nullptr,nullptr,{.visible_elapsed=std::chrono::milliseconds{5000}});
    EXPECT_EQ(adopted.phase,cooling.phase);
    EXPECT_FLOAT_EQ(adopted.phase_progress,cooling.phase_progress);
    EXPECT_EQ(adopted.surface_image->rgba,cooling.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, V10FinalIceAveragesPhysicalStocksBeforeDisplayClassification)
{
    constexpr size_t COUNT=256U*256U;
    auto snapshot=std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage=generation::GenerationStage::V10Water;
    snapshot->resolution=256U;
    auto& fields=snapshot->v10.emplace();
    fields.height_raw.assign(COUNT,1.0F);
    fields.source_elevation_m.assign(COUNT,-100.0);
    fields.plate_ids.assign(COUNT,1U);
    fields.plate_directions={{1U,0.0F,0.0F}};
    fields.local_effective_celsius.assign(8U*32U*32U,15.0);
    fields.theta_celsius.assign(8U*32U*32U,15.0);
    fields.ground_m.assign(32U*32U,0.0);
    fields.phase_precipitation_mm_year.assign(8U*COUNT,0.0);
    fields.water_level_m.assign(COUNT,0.0F);
    fields.ocean_mask.assign(COUNT,1U);
    fields.lake_mask.assign(COUNT,0U);
    fields.river_mask.assign(COUNT,0U);
    fields.land_snow_water_equivalent_m.assign(8U*COUNT,0.0F);
    fields.glacier_thickness_m.assign(8U*COUNT,0.0F);
    fields.sea_ice_thickness_m.assign(8U*COUNT,0.0F);
    std::fill_n(fields.sea_ice_thickness_m.begin(),COUNT,.2F);
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    auto uniform=std::make_shared<generation::GenerationSnapshot>(*snapshot);
    std::fill(uniform->v10->sea_ice_thickness_m.begin(),uniform->v10->sea_ice_thickness_m.end(),.025F);
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*uniform));
    client::GenerationMapAnimator seasonal_animator,uniform_animator;
    static_cast<void>(seasonal_animator.updateNative(snapshot,nullptr,nullptr,{}));
    static_cast<void>(uniform_animator.updateNative(uniform,nullptr,nullptr,{}));
    client::GenerationMapAnimationClock clock{.visible_elapsed=std::chrono::milliseconds{1'800'000}};
    auto const seasonal_frame=seasonal_animator.updateNative(snapshot,nullptr,nullptr,clock);
    auto const uniform_frame=uniform_animator.updateNative(uniform,nullptr,nullptr,clock);
    ASSERT_NE(seasonal_frame.surface_image,nullptr);
    ASSERT_NE(uniform_frame.surface_image,nullptr);
    EXPECT_EQ(seasonal_frame.surface_image->rgba,uniform_frame.surface_image->rgba);
    EXPECT_EQ(fields.sea_ice_thickness_m.front(),.2F);
    EXPECT_EQ(fields.sea_ice_thickness_m.back(),0.0F);

    auto all_phase = std::make_shared<generation::GenerationSnapshot>(*uniform);
    std::fill(all_phase->v10->sea_ice_thickness_m.begin(), all_phase->v10->sea_ice_thickness_m.end(), .2F);
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*all_phase));
    EXPECT_EQ(all_phase->v10->height_raw, uniform->v10->height_raw);
    EXPECT_EQ(all_phase->v10->source_elevation_m, uniform->v10->source_elevation_m);
    EXPECT_EQ(all_phase->v10->local_effective_celsius, uniform->v10->local_effective_celsius);
    EXPECT_EQ(all_phase->v10->theta_celsius, uniform->v10->theta_celsius);
    EXPECT_EQ(all_phase->v10->ground_m, uniform->v10->ground_m);
    EXPECT_EQ(all_phase->v10->water_level_m, uniform->v10->water_level_m);

    client::GenerationMapAnimator above_cutoff_animator;
    auto const initial = above_cutoff_animator.updateNative(all_phase, nullptr, nullptr, {});
    ASSERT_NE(initial.image, nullptr);
    auto const above_cutoff_frame = above_cutoff_animator.updateNative(all_phase, nullptr, nullptr, clock);
    ASSERT_NE(above_cutoff_frame.surface_image, nullptr);
    EXPECT_NE(above_cutoff_frame.surface_image->rgba, uniform_frame.surface_image->rgba);
    EXPECT_FLOAT_EQ(all_phase->v10->sea_ice_thickness_m.front(), .2F);
    EXPECT_FLOAT_EQ(all_phase->v10->sea_ice_thickness_m.back(), .2F);
    EXPECT_FLOAT_EQ(uniform->v10->sea_ice_thickness_m.front(), .025F);
    EXPECT_FLOAT_EQ(uniform->v10->sea_ice_thickness_m.back(), .025F);
}

TEST(GenerationHistoryRenderingTest, V10RiverFootprintStaysContinuousThroughWaterAndIce)
{
    constexpr size_t COUNT = 256U * 256U;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V10Water;
    snapshot->resolution = 256U;
    auto& fields = snapshot->v10.emplace();
    fields.height_raw.assign(COUNT, 1.0F);
    fields.source_elevation_m.assign(COUNT, 200.0);
    fields.plate_ids.assign(COUNT, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.local_effective_celsius.assign(8U * 32U * 32U, 40.0);
    fields.theta_celsius.assign(8U * 32U * 32U, 40.0);
    fields.ground_m.assign(32U * 32U, 0.0);
    fields.phase_precipitation_mm_year.assign(8U * COUNT, 0.0);
    fields.water_level_m.assign(COUNT, 200.0F);
    fields.ocean_mask.assign(COUNT, 0U);
    fields.lake_mask.assign(COUNT, 0U);
    fields.river_mask.assign(COUNT, 0U);
    fields.land_snow_water_equivalent_m.assign(8U * COUNT, 0.0F);
    fields.glacier_thickness_m.assign(8U * COUNT, 0.0F);
    fields.sea_ice_thickness_m.assign(8U * COUNT, 0.0F);
    for (uint32_t x = 32U; x < 224U; ++x)
    {
        size_t const i = 128U * 256U + x;
        fields.river_mask[i] = 1U;
        fields.water_level_m[i] = x % 3U == 0U ? 200.2F : x % 3U == 1U ? 200.0F : 199.9F;
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    auto const physical_before = fields;
    auto dry = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    std::fill(dry->v10->river_mask.begin(), dry->v10->river_mask.end(), 0U);
    client::GenerationMapAnimator animator, dry_animator;
    ASSERT_TRUE(animator.updateNative(snapshot, nullptr, nullptr, {}).surface_image);
    ASSERT_TRUE(dry_animator.updateNative(dry, nullptr, nullptr, {}).surface_image);
    auto const before_water = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{10'000}});
    auto const dry_before_water = dry_animator.updateNative(dry, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{10'000}});
    ASSERT_TRUE(before_water.surface_image);
    ASSERT_TRUE(dry_before_water.surface_image);
    EXPECT_EQ(before_water.surface_image->rgba, dry_before_water.surface_image->rgba);
    ASSERT_TRUE(animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{18'000}}).surface_image);
    for (int64_t elapsed : {28'000, 40'000})
    {
        auto const frame = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        ASSERT_TRUE(frame.surface_image);
        EXPECT_EQ(frame.phase, elapsed == 28'000 ? client::GenerationMapAnimationPhase::Water
            : client::GenerationMapAnimationPhase::Ice);
        if (char const* directory = std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR"))
        {
            writeImage(std::filesystem::path(directory) / ("v10-river-" + std::to_string(elapsed) + ".ppm"),
                *frame.surface_image);
        }
        auto const& image = *frame.surface_image;
        auto const reference = image.rgba[128U * 2U * image.width + 32U * 2U];
        EXPECT_GT(reference[2U], reference[0U]);
        for (uint32_t x = 33U; x < 224U; ++x)
        {
            auto const pixel = image.rgba[128U * 2U * image.width + x * 2U];
            for (size_t channel = 0U; channel < 3U; ++channel)
                EXPECT_NEAR(pixel[channel], reference[channel], 1.0) << "x=" << x << " elapsed=" << elapsed;
        }
    }
    EXPECT_EQ(fields, physical_before);
}

TEST(GenerationHistoryRenderingTest, V10AnnualColdInlandProjectionUsesForcingWithoutChangingPhysicalStocks)
{
    constexpr size_t COUNT = 256U * 256U;
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::V10Water;
    snapshot->resolution = 256U;
    auto& fields = snapshot->v10.emplace();
    fields.height_raw.assign(COUNT, 1.0F);
    fields.source_elevation_m.assign(COUNT, 0.0);
    fields.plate_ids.assign(COUNT, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.local_effective_celsius.assign(8U * 32U * 32U, -10.0);
    std::fill_n(fields.local_effective_celsius.begin(), 32U * 32U, 2.0);
    fields.theta_celsius.assign(8U * 32U * 32U, 15.0);
    fields.ground_m.assign(32U * 32U, 0.0);
    fields.phase_precipitation_mm_year.assign(8U * COUNT, 0.0);
    fields.water_level_m.assign(COUNT, 10.0F);
    fields.ocean_mask.assign(COUNT, 0U);
    fields.lake_mask.assign(COUNT, 1U);
    fields.river_mask.assign(COUNT, 0U);
    fields.land_snow_water_equivalent_m.assign(8U * COUNT, 0.0F);
    fields.glacier_thickness_m.assign(8U * COUNT, 0.0F);
    fields.sea_ice_thickness_m.assign(8U * COUNT, 0.0F);
    auto const physical_before = fields;
    auto warm = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    warm->v10->local_effective_celsius.assign(8U * 32U * 32U, 15.0);
    client::GenerationMapAnimator cold_animator, warm_animator;
    ASSERT_TRUE(cold_animator.updateNative(snapshot, nullptr, nullptr, {}).surface_image);
    ASSERT_TRUE(warm_animator.updateNative(warm, nullptr, nullptr, {}).surface_image);
    client::GenerationMapAnimationClock const endpoint{.visible_elapsed = std::chrono::milliseconds{1'800'000}};
    auto cold_frame = cold_animator.updateNative(snapshot, nullptr, nullptr, endpoint);
    auto warm_frame = warm_animator.updateNative(warm, nullptr, nullptr, endpoint);
    ASSERT_TRUE(cold_frame.surface_image);
    ASSERT_TRUE(warm_frame.surface_image);
    EXPECT_NE(cold_frame.surface_image->rgba, warm_frame.surface_image->rgba);
    auto const material = client::s2NativeTerrainColor(0.0F, {0.0F, 0.0F}, {0.0F, 0.0F},
        true, 10.0F, client::S2_MAP_PROJECTED_INLAND_ICE);
    for (size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_EQ(cold_frame.surface_image->rgba.front()[channel], static_cast<uint8_t>(material[channel] + .5F));
    }
    EXPECT_EQ(fields, physical_before);
}

TEST(GenerationHistoryRenderingTest, V10ThinColdSnowMatchesBulkSnowWithoutChangingTheSource)
{
    constexpr size_t COUNT = 256U * 256U;
    constexpr size_t WITNESS = 96U * 256U + 160U;
    constexpr double THIN_SNOW_M = 0.018'859;
    constexpr double GLACIER_M = 0.005'096;
    auto thin = std::make_shared<generation::GenerationSnapshot>();
    thin->stage = generation::GenerationStage::V10Water;
    thin->resolution = 256U;
    auto& fields = thin->v10.emplace();
    fields.height_raw.assign(COUNT, 1.0F);
    fields.source_elevation_m.assign(COUNT, 0.0);
    fields.plate_ids.assign(COUNT, 1U);
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.local_effective_celsius.assign(8U * 32U * 32U, -5.0);
    fields.theta_celsius.assign(8U * 32U * 32U, -5.0);
    fields.ground_m.assign(32U * 32U, 0.0);
    fields.phase_precipitation_mm_year.assign(8U * COUNT, 0.0);
    fields.water_level_m.assign(COUNT, 0.0F);
    fields.ocean_mask.assign(COUNT, 0U);
    fields.lake_mask.assign(COUNT, 0U);
    fields.river_mask.assign(COUNT, 0U);
    fields.land_snow_water_equivalent_m.assign(8U * COUNT, 0.0F);
    fields.glacier_thickness_m.assign(8U * COUNT, 0.0F);
    fields.sea_ice_thickness_m.assign(8U * COUNT, 0.0F);
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        fields.land_snow_water_equivalent_m[phase * COUNT + WITNESS] = static_cast<float>(THIN_SNOW_M);
        fields.glacier_thickness_m[phase * COUNT + WITNESS] = static_cast<float>(GLACIER_M);
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*thin));

    auto bulk = std::make_shared<generation::GenerationSnapshot>(*thin);
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        bulk->v10->land_snow_water_equivalent_m[phase * COUNT + WITNESS] = 0.2F;
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*bulk));
    EXPECT_EQ(thin->v10->height_raw, bulk->v10->height_raw);
    EXPECT_EQ(thin->v10->source_elevation_m, bulk->v10->source_elevation_m);
    EXPECT_EQ(thin->v10->local_effective_celsius, bulk->v10->local_effective_celsius);
    EXPECT_EQ(thin->v10->theta_celsius, bulk->v10->theta_celsius);
    EXPECT_EQ(thin->v10->ground_m, bulk->v10->ground_m);
    EXPECT_EQ(thin->v10->glacier_thickness_m, bulk->v10->glacier_thickness_m);
    EXPECT_EQ(thin->v10->sea_ice_thickness_m, bulk->v10->sea_ice_thickness_m);
    EXPECT_EQ(thin->v10->water_level_m, bulk->v10->water_level_m);

    generation::V10SurfaceSample thin_mean;
    thin_mean.temperature_celsius = -5.0;
    thin_mean.snow_water_equivalent_m = THIN_SNOW_M;
    thin_mean.glacier_thickness_m = GLACIER_M;
    generation::V10SurfaceSample bulk_mean = thin_mean;
    bulk_mean.snow_water_equivalent_m = 0.2;
    EXPECT_EQ(generation::classifyV10AnnualMeanSurfaceCover(thin_mean), generation::V10SurfaceCover::Snow);
    EXPECT_EQ(generation::classifyV10AnnualMeanSurfaceCover(bulk_mean), generation::V10SurfaceCover::Snow);

    auto bare = std::make_shared<generation::GenerationSnapshot>(*thin);
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        bare->v10->land_snow_water_equivalent_m[phase * COUNT + WITNESS] = 0.0F;
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*bare));

    client::GenerationMapAnimator thin_animator;
    client::GenerationMapAnimator bulk_animator;
    client::GenerationMapAnimator bare_animator;
    static_cast<void>(thin_animator.updateNative(thin, nullptr, nullptr, {}));
    static_cast<void>(bulk_animator.updateNative(bulk, nullptr, nullptr, {}));
    static_cast<void>(bare_animator.updateNative(bare, nullptr, nullptr, {}));
    client::GenerationMapAnimationClock const clock{
        .visible_elapsed = std::chrono::milliseconds{1'800'000},
    };
    auto const thin_frame = thin_animator.updateNative(thin, nullptr, nullptr, clock);
    auto const bulk_frame = bulk_animator.updateNative(bulk, nullptr, nullptr, clock);
    auto const bare_frame = bare_animator.updateNative(bare, nullptr, nullptr, clock);
    ASSERT_NE(thin_frame.surface_image, nullptr);
    ASSERT_NE(bulk_frame.surface_image, nullptr);
    ASSERT_NE(bare_frame.surface_image, nullptr);
    EXPECT_EQ(thin_frame.surface_image->rgba, bulk_frame.surface_image->rgba);
    EXPECT_NE(thin_frame.surface_image->rgba, bare_frame.surface_image->rgba);
    EXPECT_FLOAT_EQ(thin->v10->land_snow_water_equivalent_m[WITNESS], static_cast<float>(THIN_SNOW_M));
    EXPECT_FLOAT_EQ(bulk->v10->land_snow_water_equivalent_m[WITNESS], 0.2F);
}

TEST(GenerationHistoryRenderingTest, MissingSourceKeepsTheApprovedScreenshotInsteadOfAStaticLavaHold)
{
    client::GenerationMapAnimator animator;
    for (int64_t elapsed : {0, 1000, 5000})
        EXPECT_EQ(animator.updateNative(nullptr, nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}}).image, nullptr);
}

TEST(GenerationHistoryRenderingTest, MismatchedFinalIdentityLeavesTheScreenshotUntilARealSourceExists)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.model_tag = generation::TerrainModelTag::V21;
    auto source = std::make_shared<client::S2MapImage>();
    source->source_identity.terrain_version = generation::terrainVersion(atlas->configuration.model_tag);
    source->source_identity.seed = atlas->configuration.seed + 1U;
    source->source_identity.configuration_identity = generation::configurationIdentity(atlas->configuration);
    client::GenerationMapAnimator animator;
    auto rejected = animator.updateNative(atlas, source, {});
    EXPECT_EQ(rejected.image, nullptr); EXPECT_EQ(rejected.surface_image, nullptr);
}

TEST(GenerationHistoryRenderingTest, LivePlatecRunsOneVisibleClockBeforeReadinessAndWithoutAnAtlas)
{
    client::GenerationMapAnimator animator;
    auto source = livePlatecSnapshot();
    auto first = animator.updateNative(source, nullptr, nullptr, {});
    auto moving = animator.updateNative(source, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{1000}, .ready=false});
    auto cooling = animator.updateNative(source, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{5000}, .ready=false});
    ASSERT_NE(first.image, nullptr); ASSERT_NE(moving.image, nullptr); ASSERT_NE(cooling.image, nullptr);
    EXPECT_NE(first.image->rgba, moving.image->rgba);
    EXPECT_NE(moving.image->rgba, cooling.image->rgba);
    EXPECT_GT(cooling.phase_progress, 0.0F);
}

TEST(GenerationHistoryRenderingTest, NearAxisPlateMotionKeepsPeriodicSamplesInsideTheGrid)
{
    auto snapshot = livePlatecSnapshot();
    // A near-axis unit direction produces a tiny negative displacement at row
    // zero. Its periodic coordinate must stay below the exclusive grid bound.
    snapshot->v21->plate_directions = {{1U, 1.0F, 0.000'001F}};
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    client::GenerationMapAnimator animator;
    ASSERT_NE(animator.updateNative(snapshot, nullptr, nullptr, {}).image, nullptr);
    for (int64_t elapsed : {1, 42, 84})
    {
        auto const frame = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        ASSERT_NE(frame.image, nullptr);
        EXPECT_EQ(frame.image->rgba.size(), 512U * 512U);
    }
}

TEST(GenerationHistoryRenderingTest, LateTerrainAdoptsTheCurrentScalarSurfaceWithoutRestartingTheFilm)
{
    client::GenerationMapAnimator animator;
    auto source = livePlatecSnapshot();
    static_cast<void>(animator.updateNative(source, nullptr, nullptr, {}));
    auto before = animator.updateNative(source, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{5000}});
    auto later = std::make_shared<generation::GenerationSnapshot>(*source);
    later->stage = generation::GenerationStage::V21Terrain;
    for (float& h : later->v21->source_elevation_m) h *= 1.8F;
    auto adopted = animator.updateNative(later, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{5000}});
    ASSERT_NE(adopted.image, nullptr);
    EXPECT_EQ(adopted.phase, before.phase); EXPECT_EQ(adopted.phase_progress, before.phase_progress);
    EXPECT_EQ(adopted.image->rgba, before.image->rgba);
    auto ready = animator.updateNative(later, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{5000}, .ready=true});
    EXPECT_EQ(ready.image, adopted.image);
    EXPECT_EQ(ready.phase, adopted.phase);
    EXPECT_EQ(ready.phase_progress, adopted.phase_progress);
    auto continued = animator.updateNative(later, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{6000}});
    ASSERT_NE(continued.image, nullptr); EXPECT_NE(continued.image->rgba, adopted.image->rgba);
    EXPECT_NE(continued.phase, client::GenerationMapAnimationPhase::Creation);
}

TEST(GenerationHistoryRenderingTest, CellCentrePlatecUsesTheSameCoordinatesAsLaterCanonicalVertices)
{
    auto centres = livePlatecSnapshot();
    centres->v21->plate_directions = {{1U, 0.0F, 0.0F}};
    auto vertices = std::make_shared<generation::GenerationSnapshot>(*centres);
    vertices->stage = generation::GenerationStage::V21Terrain;
    for (uint32_t y = 0U; y < 256U; ++y) for (uint32_t x = 0U; x < 256U; ++x)
    {
        auto const& h = centres->v21->source_elevation_m;
        uint32_t const west = (x + 255U) % 256U, north = (y + 255U) % 256U;
        vertices->v21->source_elevation_m[y * 256U + x] =
            (h[y * 256U + x] + h[y * 256U + west] + h[north * 256U + x] + h[north * 256U + west]) * .25F;
    }
    client::GenerationMapAnimator provisional;
    auto const provisional_frame = provisional.updateNative(centres, nullptr, nullptr, {});
    ASSERT_NE(provisional_frame.surface_image, nullptr);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            auto const sample = provisional.thermalSample(x, y);
            ASSERT_TRUE(sample.has_value());
            EXPECT_FLOAT_EQ(sample->elevation_m, vertices->v21->source_elevation_m[y * 256U + x]);
        }
    }

    client::GenerationMapAnimator b;
    static_cast<void>(provisional.updateNative(vertices, nullptr, nullptr, {}));
    static_cast<void>(b.updateNative(vertices, nullptr, nullptr, {}));
    auto const cooled_clock = client::GenerationMapAnimationClock{
        .visible_elapsed = std::chrono::milliseconds{9'000},
    };
    auto const ca = provisional.updateNative(vertices, nullptr, nullptr, cooled_clock);
    auto const vb = b.updateNative(vertices, nullptr, nullptr, cooled_clock);
    ASSERT_NE(ca.surface_image, nullptr);
    ASSERT_NE(vb.surface_image, nullptr);
    EXPECT_EQ(ca.surface_image->rgba, vb.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, RealV21PreparationStartsTheFilmBeforeAnAtlasExists)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V21;
    std::stop_source stop;
    client::GenerationMapAnimator animator;
    client::GenerationMapImageHandle first;
    bool changed = false;
    auto const started = std::chrono::steady_clock::now();
    bool cancelled = false;
    try
    {
        static_cast<void>(generation::prepareAtlas(configuration, stop.get_token(), [&](auto const& progress) {
        if (!progress.snapshot || progress.stage != generation::GenerationStage::V21Platec) return;
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        auto frame = animator.updateNative(progress.snapshot, nullptr, nullptr,
            {.visible_elapsed=elapsed, .ready=false});
        ASSERT_NE(frame.image, nullptr);
        if (!first) first = frame.image;
        else if (first->rgba != frame.image->rgba)
        {
            changed = true;
            stop.request_stop();
        }
        }));
    }
    catch (std::runtime_error const& error)
    {
        cancelled = true;
        EXPECT_STREQ(error.what(), "Terrain-v2 generation was cancelled");
    }
    EXPECT_TRUE(cancelled);
    EXPECT_NE(first, nullptr);
    EXPECT_TRUE(changed);
}

std::shared_ptr<generation::GenerationSnapshot> weatherSnapshot(bool wind, bool currents)
{
    auto snapshot = livePlatecSnapshot();
    snapshot->stage = generation::GenerationStage::V21Water;
    auto& fields = *snapshot->v21;
    constexpr size_t COUNT = 256U * 256U;
    fields.plate_directions = {{1U, 0.0F, 0.0F}};
    fields.temperature_celsius.assign(COUNT, 15.0F);
    fields.precipitation_mm_year.assign(COUNT, 0.0F);
    fields.wind_u.assign(COUNT, wind ? 2.0F : 0.0F);
    fields.wind_v.assign(COUNT, 0.0F);
    fields.current_u.assign(COUNT, currents ? 1.0F : 0.0F);
    fields.current_v.assign(COUNT, 0.0F);
    fields.phase_temperature_celsius.assign(COUNT * 8U, 15.0F);
    fields.phase_precipitation_mm_year.assign(COUNT * 8U, 0.0F);
    fields.land_snow_water_equivalent_m.assign(COUNT * 8U, 0.0F);
    fields.glacier_thickness_m.assign(COUNT * 8U, 0.0F);
    fields.sea_ice_thickness_m.assign(COUNT * 8U, 0.0F);
    fields.water_level_blocks.assign(COUNT, 0.0F);
    fields.ocean_mask.resize(COUNT);
    fields.lake_mask.assign(COUNT, 0U);
    fields.river_mask.assign(COUNT, 0U);
    fields.biome_id.assign(COUNT, 0U);
    fields.ice_surface_kind.assign(COUNT, 0U);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            fields.source_elevation_m[y * 256U + x] = x < 128U ? -800.0F : 200.0F;
            fields.ocean_mask[y * 256U + x] = x < 128U ? 1U : 0U;
        }
    }
    return snapshot;
}

TEST(GenerationHistoryRenderingTest, MaterialGrainTilesExactlyAtBothWorldSeams)
{
    for(float coordinate:{0.0F,.25F,17.5F,127.0F,255.75F})
    {
        EXPECT_FLOAT_EQ(client::generationMapMaterialGrain(0.0F,coordinate),
            client::generationMapMaterialGrain(256.0F,coordinate));
        EXPECT_FLOAT_EQ(client::generationMapMaterialGrain(coordinate,0.0F),
            client::generationMapMaterialGrain(coordinate,256.0F));
        EXPECT_FLOAT_EQ(client::generationMapMaterialGrain(-.25F,coordinate),
            client::generationMapMaterialGrain(255.75F,coordinate));
        EXPECT_FLOAT_EQ(client::generationMapMaterialGrain(coordinate,-.25F),
            client::generationMapMaterialGrain(coordinate,255.75F));
        // The warped domain may cross a lattice slope at the seam; this bound
        // remains far below one displayed palette level for a .002-cell interval.
        EXPECT_NEAR(client::generationMapMaterialGrain(.001F,coordinate),
            client::generationMapMaterialGrain(255.999F,coordinate),.001F);
        EXPECT_NEAR(client::generationMapMaterialGrain(coordinate,.001F),
            client::generationMapMaterialGrain(coordinate,255.999F),.001F);
    }
}

TEST(GenerationHistoryRenderingTest, MoltenMaterialDoesNotRepeatEveryQuarterWorld)
{
    client::GenerationMapAnimator animator;
    auto frame = animator.updateNative(livePlatecSnapshot(), nullptr, nullptr, {});
    ASSERT_TRUE(frame.surface_image);
    size_t horizontal = 0U, vertical = 0U;
    for (uint32_t y = 0U; y < 128U; ++y)
    {
        for (uint32_t x = 0U; x < 128U; ++x)
        {
            auto const& pixels = frame.surface_image->rgba;
            size_t const i = y * frame.surface_image->width + x;
            horizontal += pixels[i] != pixels[i + 128U] ? 1U : 0U;
            vertical += pixels[i] != pixels[i + 128U * frame.surface_image->width] ? 1U : 0U;
        }
    }
    EXPECT_GT(horizontal, 128U * 128U / 4U);
    EXPECT_GT(vertical, 128U * 128U / 4U);
}

TEST(GenerationHistoryRenderingTest, PlateContactHeatPotentialHasNoHardDistanceCutoff)
{
    auto plain = livePlatecSnapshot();
    plain->stage = generation::GenerationStage::V21Terrain;
    plain->v21->source_elevation_m.assign(256U * 256U, 200.0F);
    plain->v21->plate_directions = {{1U, 0.0F, 0.0F}, {2U, 0.0F, 0.0F}};
    auto contacts = std::make_shared<generation::GenerationSnapshot>(*plain);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 128U; x < 256U; ++x)
        {
            contacts->v21->plate_ids[y * 256U + x] = 2U;
        }
    }
    client::GenerationMapAnimator plain_animator, contact_animator;
    ASSERT_TRUE(plain_animator.updateNative(plain, nullptr, nullptr, {}).surface_image);
    ASSERT_TRUE(contact_animator.updateNative(contacts, nullptr, nullptr, {}).surface_image);
    float previous = 0.0F;
    for (uint32_t x = 128U; x <= 140U; ++x)
    {
        auto base = plain_animator.thermalSample(x, 128U);
        auto heated = contact_animator.thermalSample(x, 128U);
        ASSERT_TRUE(base);
        ASSERT_TRUE(heated);
        ASSERT_GT(base->potential, 0.0F);
        float const ratio = heated->potential / base->potential;
        if (x > 128U)
        {
            EXPECT_GE(ratio, previous - .0001F);
            EXPECT_LT(ratio - previous, .30F);
        }
        previous = ratio;
    }
    EXPECT_FLOAT_EQ(previous, 1.0F);
}

TEST(GenerationHistoryRenderingTest, PoolRetentionFollowsAdoptedLocalDepressions)
{
    auto initial = livePlatecSnapshot();
    initial->v21->source_elevation_m.assign(256U * 256U, -500.0F);
    initial->v21->source_elevation_m.back() = 500.0F;
    auto plain = std::make_shared<generation::GenerationSnapshot>(*initial);
    plain->stage = generation::GenerationStage::V21Terrain;
    auto valleys = std::make_shared<generation::GenerationSnapshot>(*plain);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            if (x % 8U == 4U || y % 8U == 4U)
            {
                valleys->v21->source_elevation_m[y * 256U + x] = 250.0F;
            }
        }
    }
    client::GenerationMapAnimator flat_animator, valley_animator;
    ASSERT_TRUE(flat_animator.updateNative(initial, nullptr, nullptr, {}).surface_image);
    ASSERT_TRUE(valley_animator.updateNative(initial, nullptr, nullptr, {}).surface_image);
    ASSERT_TRUE(flat_animator.updateNative(plain, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'000}}).surface_image);
    ASSERT_TRUE(valley_animator.updateNative(valleys, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'000}}).surface_image);
    ASSERT_TRUE(flat_animator.updateNative(plain, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'900}}).surface_image);
    ASSERT_TRUE(valley_animator.updateNative(valleys, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'900}}).surface_image);
    size_t retained = 0U;
    for (uint32_t y = 0U; y < 256U; y += 8U)
    {
        for (uint32_t x = 0U; x < 256U; x += 8U)
        {
            auto flat = flat_animator.thermalSample(x, y);
            auto valley = valley_animator.thermalSample(x, y);
            ASSERT_TRUE(flat);
            ASSERT_TRUE(valley);
            EXPECT_FLOAT_EQ(flat->elevation_m, valley->elevation_m);
            EXPECT_LE(valley->potential, flat->potential);
            retained += valley->potential < flat->potential ? 1U : 0U;
        }
    }
    EXPECT_GT(retained, 0U);
}

double shiftedSurfaceError(client::GenerationMapImage const& before,
    client::GenerationMapImage const& after, uint32_t shift)
{
    double error=0.0;
    for(uint32_t y=0U;y<before.height;++y)
    {
        for(uint32_t x=0U;x<before.width;++x)
        {
            auto const& a=before.rgba[y*before.width+x];
            auto const& b=after.rgba[y*after.width+(x+shift)%after.width];
            for(uint32_t channel=0U;channel<3U;++channel)
            {
                error+=std::abs(static_cast<int>(a[channel])-static_cast<int>(b[channel]));
            }
        }
    }
    return error/static_cast<double>(before.rgba.size()*3U);
}

TEST(GenerationHistoryRenderingTest, CooledFieldWaitKeepsSameGeographyWithoutIllustrativeDrift)
{
    auto snapshot=livePlatecSnapshot();
    snapshot->stage=generation::GenerationStage::V21Terrain;
    snapshot->v21->plate_directions={{1U,1.0F,0.0F}};
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot,nullptr,nullptr,{}));
    auto before=animator.updateNative(snapshot,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{9'000}});
    ASSERT_TRUE(before.surface_image);
    if (char const* directory=std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR"))
    {
        std::filesystem::create_directories(directory);
        writeImage(std::filesystem::path(directory)/"dry-9000.ppm",*before.surface_image);
        for(int64_t elapsed:{10'000,12'000})
        {
            auto frame=animator.updateNative(snapshot,nullptr,nullptr,
                {.visible_elapsed=std::chrono::milliseconds{elapsed}});
            ASSERT_TRUE(frame.surface_image);
            writeImage(std::filesystem::path(directory)/("dry-"+std::to_string(elapsed)+".ppm"),*frame.surface_image);
        }
    }
    auto after=animator.updateNative(snapshot,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{14'000}});
    ASSERT_TRUE(after.surface_image);
    if (char const* directory=std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR"))
    {
        writeImage(std::filesystem::path(directory)/"dry-14000.ppm",*after.surface_image);
    }
    EXPECT_EQ(before.surface_image->rgba,after.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, LateWaterAdoptionCannotReverseStationaryGeography)
{
    auto snapshot=livePlatecSnapshot();
    snapshot->stage=generation::GenerationStage::V21Terrain;
    snapshot->v21->plate_directions={{1U,1.0F,0.0F}};
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot,nullptr,nullptr,{}));
    auto before=animator.updateNative(snapshot,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{14'000}});
    auto water=weatherSnapshot(false,false);
    water->v21->source_elevation_m=snapshot->v21->source_elevation_m;
    std::fill(water->v21->ocean_mask.begin(),water->v21->ocean_mask.end(),0U);
    auto adopted=animator.updateNative(water,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{14'000}});
    ASSERT_TRUE(before.surface_image);
    ASSERT_TRUE(adopted.surface_image);
    EXPECT_EQ(before.surface_image->rgba,adopted.surface_image->rgba);
    auto adjacent=animator.updateNative(water,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{14'042}});
    ASSERT_TRUE(adjacent.surface_image);
    EXPECT_LT(shiftedSurfaceError(*adopted.surface_image,*adjacent.surface_image,0U),1.0);
    if (char const* directory=std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR"))
    {
        std::filesystem::create_directories(directory);
        auto const output=std::filesystem::path(directory);
        writeImage(output/"adopt-0.ppm",*adopted.surface_image);
        writeImage(output/"adopt-42.ppm",*adjacent.surface_image);
        for(int64_t delta:{200,2'000})
        {
            auto frame=animator.updateNative(water,nullptr,nullptr,
                {.visible_elapsed=std::chrono::milliseconds{14'000+delta}});
            ASSERT_TRUE(frame.surface_image);
            writeImage(output/("adopt-"+std::to_string(delta)+".ppm"),*frame.surface_image);
        }
    }
    auto settled=animator.updateNative(water,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{16'100}});
    ASSERT_TRUE(settled.surface_image);
    // Hydrology cannot run a previously displaced surface backward toward its source.
    EXPECT_EQ(settled.surface_image->rgba,adopted.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, WindGlyphsDoNotJumpAtTheFormerFourSecondReset)
{
    auto snapshot = weatherSnapshot(true, false);
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    auto before = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{83'950}});
    auto after = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{84'000}});
    auto repeated = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{84'000}});
    ASSERT_NE(before.image, nullptr);
    ASSERT_NE(after.image, nullptr);
    ASSERT_NE(repeated.image, nullptr);
    ASSERT_NE(before.surface_image, nullptr);
    ASSERT_NE(after.surface_image, nullptr);
    ASSERT_NE(repeated.surface_image, nullptr);
    EXPECT_EQ(after.image->rgba, repeated.image->rgba);
    EXPECT_EQ(after.surface_image->rgba, repeated.surface_image->rgba);
    ASSERT_EQ(before.image->width, after.image->width);
    ASSERT_EQ(before.image->height, after.image->height);
    ASSERT_EQ(before.image->rgba.size(), after.image->rgba.size());
    ASSERT_GT(after.image->width, 1U);
    size_t before_count = 0U, after_count = 0U, shared_count = 0U;
    double rgb_difference = 0.0;
    std::vector<uint8_t> before_mask(before.image->rgba.size());
    std::vector<uint8_t> after_mask(after.image->rgba.size());
    for (size_t i = 0U; i < before.image->rgba.size(); ++i)
    {
        bool const b = before.image->rgba[i] != before.surface_image->rgba[i];
        bool const a = after.image->rgba[i] != after.surface_image->rgba[i];
        before_mask[i] = b ? 1U : 0U;
        after_mask[i] = a ? 1U : 0U;
        before_count += b ? 1U : 0U;
        after_count += a ? 1U : 0U;
        shared_count += a && b ? 1U : 0U;
        for (size_t channel = 0U; channel < 3U; ++channel)
        {
            rgb_difference += std::abs(static_cast<int32_t>(after.image->rgba[i][channel])
                - static_cast<int32_t>(after.surface_image->rgba[i][channel]));
        }
    }
    ASSERT_GT(before_count, 0U);
    ASSERT_GT(after_count, 0U);
    double const denominator = static_cast<double>(std::min(before_count, after_count));
    EXPECT_GE(static_cast<double>(shared_count) / denominator, .80);
    // A half-map teleport preserves density but must fail the same continuity metric.
    size_t jumped_shared_count = 0U;
    for (uint32_t y = 0U; y < after.image->height; ++y)
    {
        for (uint32_t x = 0U; x < after.image->width; ++x)
        {
            size_t const i = static_cast<size_t>(y) * after.image->width + x;
            size_t const jumped = static_cast<size_t>(y) * after.image->width
                + (x + after.image->width / 2U) % after.image->width;
            jumped_shared_count += before_mask[i] && after_mask[jumped] ? 1U : 0U;
        }
    }
    EXPECT_LT(static_cast<double>(jumped_shared_count) / denominator, .80);
    // The rendered RGB delta already includes alpha compositing. Faint cloud ribbons
    // occupy more pixels than opaque tracers without obscuring the same map contrast.
    double const normalized_contrast = rgb_difference
        / (static_cast<double>(after.image->rgba.size()) * 3.0 * 255.0);
    EXPECT_GT(normalized_contrast, 0.0);
    EXPECT_LE(normalized_contrast, .02);
}

TEST(GenerationHistoryRenderingTest, CurrentArrowsAndParticlesNeverColorLandEvenWithNonzeroLandVectors)
{
    auto currents = weatherSnapshot(false, true);
    auto control = weatherSnapshot(false, false);
    // Put a coastline through a moving arrow, so clipping is exercised rather
    // than merely relying on seeds that happened to start far from land.
    constexpr uint32_t COAST_X = 114U;
    for (auto const& snapshot : {currents, control})
    {
        for (uint32_t y = 0U; y < 256U; ++y)
        {
            for (uint32_t x = 0U; x < 256U; ++x)
            {
                snapshot->v21->ocean_mask[y * 256U + x] = x < COAST_X ? 1U : 0U;
                snapshot->v21->source_elevation_m[y * 256U + x] = x < COAST_X ? -800.0F : 200.0F;
            }
        }
    }
    client::GenerationMapAnimator a, b;
    static_cast<void>(a.updateNative(currents, nullptr, nullptr, {}));
    static_cast<void>(b.updateNative(control, nullptr, nullptr, {}));
    auto water = a.updateNative(currents, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{120'000}, .ready=true});
    auto still = b.updateNative(control, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{120'000}, .ready=true});
    ASSERT_NE(water.image, nullptr); ASSERT_NE(still.image, nullptr);
    ASSERT_EQ(water.phase, client::GenerationMapAnimationPhase::Climate);
    ASSERT_GT(water.phase_progress, .4F);
    size_t wet_changes = 0U;
    for (uint32_t y = 0U; y < water.image->height; ++y)
    {
        for (uint32_t x = 0U; x < water.image->width; ++x)
        {
            size_t const i = static_cast<size_t>(y) * water.image->width + x;
            if (x >= COAST_X * client::GENERATION_MAP_ANIMATION_SCALE)
            {
                EXPECT_EQ(water.image->rgba[i], still.image->rgba[i]);
            }
            else if (water.image->rgba[i] != still.image->rgba[i])
            {
                ++wet_changes;
            }
        }
    }
    EXPECT_GT(wet_changes, 0U);
    EXPECT_EQ(water.surface_image->rgba, still.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, WeatherKeepsMovingAfterReadinessInsteadOfFadingOut)
{
    auto snapshot = weatherSnapshot(true, true);
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    auto ready = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{120'000}, .ready=true});
    client::GenerationMapAnimationFrame later;
    for (int64_t elapsed = 120'050; elapsed <= 120'500; elapsed += 50)
    {
        later = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}, .ready=true});
    }
    ASSERT_NE(ready.image, nullptr); ASSERT_NE(later.image, nullptr);
    EXPECT_NE(ready.image->rgba, ready.surface_image->rgba);
    EXPECT_NE(later.image->rgba, later.surface_image->rgba);
    EXPECT_NE(ready.image->rgba, later.image->rgba);
}

TEST(GenerationHistoryRenderingTest, LongReadyWeatherKeepsBoundedWorkAndRetainsPositions)
{
    auto snapshot = weatherSnapshot(true, true);
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    for (int64_t elapsed : {70'000, 70'050, 1'800'000, 1'800'050})
    {
        if (elapsed == 1'800'000)
        {
            snapshot = std::make_shared<generation::GenerationSnapshot>(*snapshot);
        }
        auto const started = std::clock();
        auto frame = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}, .ready=true});
        std::cout << "[weather-cost] elapsed_ms=" << elapsed << " cpu_us="
            << static_cast<int64_t>(static_cast<double>(std::clock() - started) * 1'000'000.0 / CLOCKS_PER_SEC)
            << '\n';
        ASSERT_NE(frame.image, nullptr);
        if (char const* directory = std::getenv("MC_WEATHER_FIXTURE_EVIDENCE_DIR"))
        {
            std::filesystem::create_directories(directory);
            writeImage(std::filesystem::path(directory) / ("ready-" + std::to_string(elapsed) + ".ppm"), *frame.image);
        }
    }
}

TEST(GenerationHistoryRenderingTest, WindAndCurrentDecoratorsWaitUntilTheIceAgeEnds)
{
    auto snapshot = weatherSnapshot(true, true);
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    for (int64_t elapsed : {15'000, 25'000, 40'000, 50'000})
    {
        auto frame = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}});
        ASSERT_NE(frame.image, nullptr);
        EXPECT_EQ(frame.image->rgba, frame.surface_image->rgba);
        EXPECT_NE(frame.phase, client::GenerationMapAnimationPhase::Climate);
    }
    auto climate = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{80'000}});
    EXPECT_EQ(climate.phase, client::GenerationMapAnimationPhase::Climate);
    EXPECT_NE(climate.image->rgba, climate.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, LateSnapshotUsesActualBiomeIdsInsteadOfDefaultingEverywhereToSnow)
{
    auto snow=weatherSnapshot(false,false);
    auto green=std::make_shared<generation::GenerationSnapshot>(*snow);
    std::fill(green->v21->biome_id.begin(),green->v21->biome_id.end(),3U);
    client::GenerationMapAnimator snow_animator,green_animator;
    static_cast<void>(snow_animator.updateNative(snow,nullptr,nullptr,{}));
    static_cast<void>(green_animator.updateNative(green,nullptr,nullptr,{}));
    auto snowy=snow_animator.updateNative(snow,nullptr,nullptr,{.visible_elapsed=std::chrono::milliseconds{1'800'000}});
    auto grassy=green_animator.updateNative(green,nullptr,nullptr,{.visible_elapsed=std::chrono::milliseconds{1'800'000}});
    ASSERT_NE(snowy.surface_image,nullptr);
    ASSERT_NE(grassy.surface_image,nullptr);
    EXPECT_NE(snowy.surface_image->rgba,grassy.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, InlandWaterReadsAsBlueWaterBeforeAnyGreening)
{
    auto snapshot=weatherSnapshot(false,false);
    auto& fields=*snapshot->v21;
    std::fill(fields.source_elevation_m.begin(),fields.source_elevation_m.end(),200.0F);
    std::fill(fields.ocean_mask.begin(),fields.ocean_mask.end(),0U);
    std::fill(fields.lake_mask.begin(),fields.lake_mask.end(),1U);
    std::fill(fields.water_level_blocks.begin(),fields.water_level_blocks.end(),202.0F*.34F);
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot,nullptr,nullptr,{}));
    auto frame=animator.updateNative(snapshot,nullptr,nullptr,{.visible_elapsed=std::chrono::milliseconds{15'000}});
    ASSERT_EQ(frame.phase,client::GenerationMapAnimationPhase::Water);
    ASSERT_NE(frame.surface_image,nullptr);
    auto const color=frame.surface_image->rgba[256U*frame.surface_image->width+256U];
    EXPECT_GT(color[2U],color[1U]);
    EXPECT_GT(color[1U],color[0U]);
}

TEST(GenerationHistoryRenderingTest, PastIceAgeCoversTheColderHalfEvenWhenActualTemperaturesExceedTenCelsius)
{
    auto snapshot = weatherSnapshot(false, false);
    auto& fields = *snapshot->v21;
    // The warm exposed control is vegetated land, not the helper's default snow biome.
    fields.biome_id.assign(fields.source_elevation_m.size(), 2U);
    for (size_t i = 0U; i < fields.source_elevation_m.size(); ++i)
    {
        fields.source_elevation_m[i] = 200.0F;
        fields.ocean_mask[i] = 0U;
        float const temperature = 20.0F + 40.0F * static_cast<float>(i % 256U) / 256.0F;
        fields.temperature_celsius[i] = temperature;
        for (size_t phase = 0U; phase < 8U; ++phase)
        {
            fields.phase_temperature_celsius[phase * 256U * 256U + i] = temperature;
        }
    }
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    auto frame = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{40'000}});
    ASSERT_EQ(frame.phase, client::GenerationMapAnimationPhase::Ice);
    ASSERT_TRUE(frame.surface_image);
    // Use the public opaque land-ice material, not brightness shared by exposed highlands.
    auto const ice_material = client::s2NativeTerrainColor(
        200.0F, {200.0F, 200.0F}, {200.0F, 200.0F}, false, 0.0F, 2U);
    client::GenerationMapRgba8 ice_color{0U, 0U, 0U, 255U};
    for (size_t channel = 0U; channel < 3U; ++channel)
    {
        ice_color[channel] = static_cast<uint8_t>(ice_material[channel] + .5F);
    }
    size_t ice_pixels = 0U;
    for (auto const& color : frame.surface_image->rgba)
    {
        ice_pixels += color == ice_color ? 1U : 0U;
    }
    EXPECT_GT(ice_pixels, frame.surface_image->rgba.size() / 3U);
    EXPECT_LT(ice_pixels, frame.surface_image->rgba.size() * 2U / 3U);
    auto const cold = frame.surface_image->rgba[128U * 2U * frame.surface_image->width + 32U * 2U];
    auto const warm = frame.surface_image->rgba[128U * 2U * frame.surface_image->width + 224U * 2U];
    EXPECT_EQ(cold, ice_color);
    EXPECT_NE(warm, ice_color);
    bool exposed_warm_side = false;
    for (uint32_t x = 0U; x < 256U; ++x)
    {
        auto const& color = frame.surface_image->rgba[128U * 2U * frame.surface_image->width + x * 2U];
        bool const frozen = color == ice_color;
        EXPECT_FALSE(exposed_warm_side && frozen);
        exposed_warm_side = exposed_warm_side || !frozen;
    }
    EXPECT_TRUE(std::all_of(fields.glacier_thickness_m.begin(), fields.glacier_thickness_m.end(),
        [](float value) { return value == 0.0F; }));
}

TEST(GenerationHistoryRenderingTest, OneOpaqueIceFrontRetreatsToUnchangingModernStock)
{
    auto snapshot=weatherSnapshot(false,false);
    auto& fields=*snapshot->v21;
    constexpr size_t COUNT=256U*256U;
    fields.source_elevation_m.assign(COUNT,200.0F);
    fields.ocean_mask.assign(COUNT,0U);
    fields.biome_id.assign(COUNT,2U);
    fields.precipitation_mm_year.assign(COUNT,2'000.0F);
    fields.phase_precipitation_mm_year.assign(COUNT*8U,2'000.0F);
    for(size_t i=0U;i<COUNT;++i)
    {
        float const temperature=30.0F*static_cast<float>(i%256U)/255.0F;
        fields.temperature_celsius[i]=temperature;
        for(size_t phase=0U;phase<8U;++phase)
        {
            fields.phase_temperature_celsius[phase*COUNT+i]=temperature;
            fields.land_snow_water_equivalent_m[phase*COUNT+i]=i%256U<32U ? 1.0F : 0.0F;
        }
    }
    auto pixel=[](client::GenerationMapAnimationFrame const& frame,uint32_t x) {
        return frame.surface_image->rgba[100U*2U*frame.surface_image->width+x*2U];
    };
    client::GenerationMapAnimator animator;
    ASSERT_TRUE(animator.updateNative(snapshot,nullptr,nullptr,{}).surface_image);
    auto peak=animator.updateNative(snapshot,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{40'000}});
    ASSERT_TRUE(peak.surface_image);
    auto const stock=pixel(peak,16U);
    auto const extra=pixel(peak,80U);
    std::vector<client::GenerationMapRgba8> retreat_pixels;
    for(int64_t elapsed:{45'000,50'000,55'000,60'000,70'000,90'000})
    {
        auto frame=animator.updateNative(snapshot,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}});
        ASSERT_TRUE(frame.surface_image);
        EXPECT_EQ(pixel(frame,16U),stock);
        retreat_pixels.push_back(pixel(frame,80U));
    }
    auto const bare=retreat_pixels.back();
    ASSERT_NE(extra,bare);
    for(auto const& color:retreat_pixels)
    {
        // A point is covered by the one opaque front or exposed, never an alpha fade.
        EXPECT_TRUE(color==extra || color==bare);
    }
    EXPECT_EQ(retreat_pixels.front(),extra);
    EXPECT_EQ(retreat_pixels.back(),bare);
}

TEST(GenerationHistoryRenderingTest, RisingWaterHeadsFillDeepOceanAndSeparateLakesBeforeTheirShallows)
{
    auto snapshot = weatherSnapshot(false, false);
    auto& fields = *snapshot->v21;
    fields.source_elevation_m.assign(256U * 256U, 400.0F);
    fields.ocean_mask.assign(256U * 256U, 0U);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 128U; ++x)
        {
            fields.source_elevation_m[y * 256U + x] = x < 64U ? -1'000.0F : -100.0F;
            fields.ocean_mask[y * 256U + x] = 1U;
        }
    }
    for (uint32_t y = 80U; y < 112U; ++y)
    {
        for (uint32_t x = 160U; x < 192U; ++x)
        {
            fields.source_elevation_m[y * 256U + x] = x < 176U ? 100.0F : 180.0F;
            fields.water_level_blocks[y * 256U + x] = 68.0F;
            fields.lake_mask[y * 256U + x] = 1U;
        }
        for (uint32_t x = 208U; x < 240U; ++x)
        {
            fields.source_elevation_m[y * 256U + x] = x < 224U ? 300.0F : 340.0F;
            fields.water_level_blocks[y * 256U + x] = 119.0F;
            fields.lake_mask[y * 256U + x] = 1U;
        }
    }
    auto dry_source = std::make_shared<generation::GenerationSnapshot>(*snapshot);
    dry_source->v21->ocean_mask.assign(256U * 256U, 0U);
    dry_source->v21->lake_mask.assign(256U * 256U, 0U);
    auto wet = [](client::GenerationMapAnimationFrame const& frame,
        client::GenerationMapAnimationFrame const& reference, uint32_t x, uint32_t y) {
        size_t const i = (y * 2U) * frame.surface_image->width + x * 2U;
        return frame.surface_image->rgba[i] != reference.surface_image->rgba[i];
    };
    client::GenerationMapAnimator animator, dry_animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    static_cast<void>(dry_animator.updateNative(dry_source, nullptr, nullptr, {}));
    auto dry = animator.updateNative(snapshot, nullptr, nullptr, {.visible_elapsed=std::chrono::milliseconds{9'000}});
    auto dry_reference = dry_animator.updateNative(dry_source, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{9'000}});
    EXPECT_FALSE(wet(dry, dry_reference, 32U, 100U));
    auto early = animator.updateNative(snapshot, nullptr, nullptr, {.visible_elapsed=std::chrono::milliseconds{15'000}});
    auto early_reference = dry_animator.updateNative(dry_source, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{15'000}});
    EXPECT_TRUE(wet(early, early_reference, 32U, 100U));
    EXPECT_FALSE(wet(early, early_reference, 96U, 100U));
    EXPECT_TRUE(wet(early, early_reference, 168U, 100U));
    EXPECT_FALSE(wet(early, early_reference, 184U, 100U));
    EXPECT_TRUE(wet(early, early_reference, 216U, 100U));
    EXPECT_FALSE(wet(early, early_reference, 232U, 100U));
    auto late = animator.updateNative(snapshot, nullptr, nullptr, {.visible_elapsed=std::chrono::milliseconds{25'000}});
    auto late_reference = dry_animator.updateNative(dry_source, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{25'000}});
    EXPECT_TRUE(wet(late, late_reference, 96U, 100U));
    EXPECT_TRUE(wet(late, late_reference, 184U, 100U));
    EXPECT_TRUE(wet(late, late_reference, 232U, 100U));
    EXPECT_FALSE(wet(late, late_reference, 148U, 100U));
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            size_t const i = static_cast<size_t>(y) * 256U + x;
            if ((fields.ocean_mask[i] != 0U || fields.lake_mask[i] != 0U)
                && wet(early, early_reference, x, y))
            {
                EXPECT_TRUE(wet(late, late_reference, x, y));
            }
        }
    }
}

TEST(GenerationHistoryRenderingTest, HydrologyCannotRevealWaterBeforeFillingStarts)
{
    auto wet_snapshot = weatherSnapshot(true, false);
    wet_snapshot->v21->plate_directions={{1U,.8F,.2F}};
    for (uint32_t y=0U;y<256U;++y) for (uint32_t x=128U;x<256U;++x)
    {
        size_t const i=y*256U+x;
        wet_snapshot->v21->source_elevation_m[i]=200.0F-5.0F*static_cast<float>(x-128U);
        wet_snapshot->v21->river_mask[i]=x>170U && x<230U ? 1U : 0U;
        wet_snapshot->v21->water_level_blocks[i]=1'000.0F;
    }
    auto dry_snapshot = std::make_shared<generation::GenerationSnapshot>(*wet_snapshot);
    // Actual wet basin cells can differ from the advected relief even at filling=0.
    // No part of that stored basin floor may reveal water before the water phase.
    std::fill(dry_snapshot->v21->ocean_mask.begin(), dry_snapshot->v21->ocean_mask.end(), 0U);
    std::fill(dry_snapshot->v21->lake_mask.begin(), dry_snapshot->v21->lake_mask.end(), 0U);
    std::fill(dry_snapshot->v21->river_mask.begin(), dry_snapshot->v21->river_mask.end(), 0U);
    client::GenerationMapAnimator wet, dry;
    static_cast<void>(wet.updateNative(wet_snapshot, nullptr, nullptr, {}));
    static_cast<void>(dry.updateNative(dry_snapshot, nullptr, nullptr, {}));
    for (int64_t elapsed : {2'000,4'000,6'000,7'000})
    {
        auto const wet_frame=wet.updateNative(wet_snapshot,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}});
        auto const dry_frame=dry.updateNative(dry_snapshot,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}});
        ASSERT_NE(wet_frame.surface_image,nullptr);
        EXPECT_EQ(wet_frame.surface_image->rgba,dry_frame.surface_image->rgba);
    }
}

std::shared_ptr<generation::GenerationSnapshot> provisionalThermalSnapshot()
{
    auto snapshot = livePlatecSnapshot();
    snapshot->v21->plate_directions = {{1U, 0.0F, 0.0F}};
    for (float& height : snapshot->v21->source_elevation_m)
    {
        height = height > 0.0F ? -100.0F : -600.0F;
    }
    return snapshot;
}

double thermalMeanRed(client::GenerationMapAnimationFrame const& frame, uint32_t x, uint32_t y)
{
    double sum = 0.0;
    for (uint32_t dy = 0U; dy < 16U; ++dy)
    {
        for (uint32_t dx = 0U; dx < 16U; ++dx)
        {
            sum += frame.surface_image->rgba[((y + dy) * 2U) * frame.surface_image->width + (x + dx) * 2U][0U];
        }
    }
    return sum / 256.0;
}

TEST(GenerationHistoryRenderingTest, ProvisionalHighlandsCoolGraduallyBeforeBasinsEvenBelowFutureSeaLevel)
{
    auto snapshot = provisionalThermalSnapshot();
    snapshot->stage = generation::GenerationStage::V21Terrain;
    client::GenerationMapAnimator animator;
    auto initial = animator.updateNative(snapshot, nullptr, nullptr, {});
    ASSERT_NE(initial.image, nullptr);
    ASSERT_TRUE(initial.surface_image);
    auto initial_thermal = animator.thermalSample(110U, 110U);
    ASSERT_TRUE(initial_thermal);
    auto orange = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{2'000}});
    ASSERT_TRUE(orange.surface_image);
    auto orange_thermal = animator.thermalSample(110U, 110U);
    ASSERT_TRUE(orange_thermal);
    auto red = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{3'000}});
    ASSERT_TRUE(red.surface_image);
    auto red_thermal = animator.thermalSample(110U, 110U);
    ASSERT_TRUE(red_thermal);
    auto crust = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{4'500}});
    ASSERT_TRUE(crust.surface_image);
    auto crust_thermal = animator.thermalSample(110U, 110U);
    auto basin_thermal = animator.thermalSample(16U, 16U);
    ASSERT_TRUE(crust_thermal);
    ASSERT_TRUE(basin_thermal);
    EXPECT_FLOAT_EQ(initial_thermal->progress, 0.0F);
    EXPECT_GT(orange_thermal->progress, initial_thermal->progress);
    EXPECT_GT(red_thermal->progress, orange_thermal->progress);
    EXPECT_GT(crust_thermal->progress, red_thermal->progress);
    EXPECT_GT(crust_thermal->progress, basin_thermal->progress);
    // Red clips at the common bright onset; measure visible dimming only where it is not clipped.
    std::array<client::GenerationMapAnimationFrame const*, 2U> const early_frames{&initial, &orange};
    std::array<double, 2U> green_mean{};
    for (size_t sample = 0U; sample < early_frames.size(); ++sample)
    {
        auto const& image = *early_frames[sample]->surface_image;
        for (uint32_t dy = 0U; dy < 16U; ++dy)
        {
            for (uint32_t dx = 0U; dx < 16U; ++dx)
            {
                uint32_t const green = image.rgba[(110U + dy) * 2U * image.width + (110U + dx) * 2U][1U];
                ASSERT_GT(green, 0U);
                ASSERT_LT(green, 254U);
                green_mean[sample] += green;
            }
        }
        green_mean[sample] /= 256.0;
    }
    EXPECT_GT(green_mean[0U], green_mean[1U]);
    EXPECT_GT(thermalMeanRed(orange, 110U, 110U), thermalMeanRed(red, 110U, 110U));
    EXPECT_GT(thermalMeanRed(red, 110U, 110U), thermalMeanRed(crust, 110U, 110U));
    EXPECT_LT(thermalMeanRed(crust, 110U, 110U), 100.0);
    EXPECT_GT(thermalMeanRed(crust, 16U, 16U), 180.0);
    auto basins = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{8'000}});
    ASSERT_TRUE(basins.surface_image);
    EXPECT_LT(thermalMeanRed(basins, 16U, 16U), thermalMeanRed(crust, 16U, 16U));
}

TEST(GenerationHistoryRenderingTest, LaterUpliftUpdatesPotentialWithoutReheatingAccumulatedCooling)
{
    auto snapshot=provisionalThermalSnapshot();
    snapshot->stage = generation::GenerationStage::V21Terrain;
    client::GenerationMapAnimator animator;
    ASSERT_TRUE(animator.updateNative(snapshot,nullptr,nullptr,{}).surface_image);
    ASSERT_TRUE(animator.updateNative(snapshot,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{2'000}}).surface_image);
    auto before=animator.thermalSample(110U,110U);
    ASSERT_TRUE(before);
    auto uplift=std::make_shared<generation::GenerationSnapshot>(*snapshot);
    uplift->v21->source_elevation_m[1U]=10'000.0F;
    ASSERT_TRUE(animator.updateNative(uplift,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{2'000}}).surface_image);
    auto adoption=animator.thermalSample(110U,110U);
    ASSERT_TRUE(adoption);
    EXPECT_FLOAT_EQ(adoption->progress,before->progress);
    for(int64_t elapsed:{2'042,2'200,2'900,4'500,7'000,8'000})
    {
        ASSERT_TRUE(animator.updateNative(uplift,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}}).surface_image);
        auto current=animator.thermalSample(110U,110U);
        ASSERT_TRUE(current);
        EXPECT_GE(current->progress,adoption->progress);
        adoption=current;
    }
    EXPECT_FLOAT_EQ(adoption->progress,1.0F);
}

TEST(GenerationHistoryRenderingTest, ThermalPotentialUsesDisplayedAdoptionAndBreaksFlatFloorPlateaus)
{
    auto first=provisionalThermalSnapshot();
    client::GenerationMapAnimator animator;
    ASSERT_TRUE(animator.updateNative(first,nullptr,nullptr,{}).surface_image);
    ASSERT_TRUE(animator.updateNative(first,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{1'000}}).surface_image);
    std::set<int32_t> floor_potentials;
    for(uint32_t y=0U;y<40U;++y)
    {
        for(uint32_t x=0U;x<256U;++x)
        {
            auto sample=animator.thermalSample(x,y);
            ASSERT_TRUE(sample);
            EXPECT_FLOAT_EQ(sample->progress,0.0F);
            floor_potentials.insert(static_cast<int32_t>(sample->potential*100'000.0F));
        }
    }
    EXPECT_GT(floor_potentials.size(),1'000U);
    auto terrain=std::make_shared<generation::GenerationSnapshot>(*first);
    terrain->stage=generation::GenerationStage::V21Terrain;
    terrain->v21->source_elevation_m.assign(256U*256U,-500.0F);
    terrain->v21->source_elevation_m[16U*256U+176U]=497.102'57F;
    terrain->v21->source_elevation_m[37U*256U+141U]=64.273'08F;
    ASSERT_TRUE(animator.updateNative(terrain,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{1'000}}).surface_image);
    auto at_start=animator.thermalSample(176U,16U);
    ASSERT_TRUE(at_start);
    EXPECT_FLOAT_EQ(at_start->elevation_m,-600.0F);
    ASSERT_TRUE(animator.updateNative(terrain,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{1'900}}).surface_image);
    auto summit=animator.thermalSample(176U,16U),lowland=animator.thermalSample(141U,37U);
    ASSERT_TRUE(summit); ASSERT_TRUE(lowland);
    EXPECT_FLOAT_EQ(summit->elevation_m,497.102'57F);
    EXPECT_GT(summit->potential,lowland->potential);
    EXPECT_GT(summit->progress,lowland->progress);
}

void checkOrdinaryReliefArrival(int64_t arrival_ms)
{
    auto provisional=provisionalThermalSnapshot();
    auto terrain=std::make_shared<generation::GenerationSnapshot>(*provisional);
    terrain->stage=generation::GenerationStage::V21Terrain;
    for(float& height:terrain->v21->source_elevation_m)
    {
        height=height>-500.0F ? 500.0F : -500.0F;
    }
    client::GenerationMapAnimator animator;
    ASSERT_TRUE(animator.updateNative(provisional,nullptr,nullptr,{}).surface_image);
    float previous=0.0F;
    for(int64_t elapsed=250;elapsed<arrival_ms;elapsed+=250)
    {
        auto frame=animator.updateNative(provisional,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}});
        auto sample=animator.thermalSample(110U,110U);
        ASSERT_TRUE(sample); ASSERT_TRUE(frame.surface_image);
        EXPECT_GE(sample->progress,previous);
        EXPECT_LT(sample->progress,1.0F);
        EXPECT_EQ(frame.image->rgba,frame.surface_image->rgba);
        previous=sample->progress;
    }
    auto before=animator.updateNative(provisional,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{arrival_ms}});
    auto old_sample=animator.thermalSample(110U,110U);
    ASSERT_TRUE(old_sample); ASSERT_TRUE(before.surface_image);
    auto adopted=animator.updateNative(terrain,nullptr,nullptr,
        {.visible_elapsed=std::chrono::milliseconds{arrival_ms}});
    auto new_sample=animator.thermalSample(110U,110U);
    ASSERT_TRUE(new_sample); ASSERT_TRUE(adopted.surface_image);
    EXPECT_FLOAT_EQ(old_sample->progress,new_sample->progress);
    EXPECT_EQ(before.surface_image->rgba,adopted.surface_image->rgba);
    auto wet=weatherSnapshot(true,false);
    wet->v21->source_elevation_m=terrain->v21->source_elevation_m;
    wet->v21->precipitation_mm_year.assign(256U*256U,2'000.0F);
    wet->v21->phase_precipitation_mm_year.assign(256U*256U*8U,2'000.0F);
    for(int64_t delta:{42,200,900,2'000,4'000,6'000,7'000,8'000,8'350})
    {
        auto frame=animator.updateNative(delta<=900 ? terrain : wet,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{arrival_ms+delta}});
        auto sample=animator.thermalSample(110U,110U);
        ASSERT_TRUE(sample); ASSERT_TRUE(frame.surface_image);
        EXPECT_GE(sample->progress,new_sample->progress);
        EXPECT_EQ(frame.image->rgba,frame.surface_image->rgba);
        EXPECT_NE(frame.phase,client::GenerationMapAnimationPhase::Ice);
        if(delta==900)
        {
            EXPECT_FLOAT_EQ(sample->elevation_m,500.0F);
        }
        if(delta==8'000)
        {
            EXPECT_FLOAT_EQ(sample->progress,1.0F);
        }
        new_sample=sample;
    }
}

TEST(GenerationHistoryRenderingTest, ColdOrdinaryH0ArrivalCannotFinishCoolingBeforeMeaningfulRelief)
{
    checkOrdinaryReliefArrival(10'179);
}

TEST(GenerationHistoryRenderingTest, WarmOrdinaryH0ArrivalCannotFinishCoolingBeforeMeaningfulRelief)
{
    checkOrdinaryReliefArrival(6'815);
}

TEST(GenerationHistoryRenderingTest, NativePlateContactsRemainHotAfterHighlandInteriorsCrust)
{
    auto snapshot = provisionalThermalSnapshot();
    snapshot->stage = generation::GenerationStage::V21Terrain;
    snapshot->v21->plate_directions.push_back({2U, 0.0F, 0.0F});
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 128U; x < 256U; ++x)
        {
            snapshot->v21->plate_ids[y * 256U + x] = 2U;
        }
    }
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    auto frame = animator.updateNative(snapshot, nullptr, nullptr, {.visible_elapsed=std::chrono::milliseconds{5'000}});
    ASSERT_NE(frame.surface_image, nullptr);
    auto const interior = frame.surface_image->rgba[(128U * 2U) * frame.surface_image->width + 100U * 2U];
    auto const seam = frame.surface_image->rgba[(128U * 2U) * frame.surface_image->width + 128U * 2U];
    EXPECT_LT(interior[0U], 100U);
    EXPECT_GT(seam[0U], static_cast<uint32_t>(interior[0U]) + 30U);
}

TEST(GenerationHistoryRenderingTest, CoolingFinishesAcrossBasinsAndContactsAtEightSeconds)
{
    auto snapshot = provisionalThermalSnapshot();
    snapshot->stage = generation::GenerationStage::V21Terrain;
    snapshot->v21->plate_directions.push_back({2U, 0.0F, 0.0F});
    for (uint32_t y = 0U; y < 256U; ++y)
        for (uint32_t x = 128U; x < 256U; ++x)
            snapshot->v21->plate_ids[y * 256U + x] = 2U;
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    auto hot = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{5'000}});
    auto nearly_solid = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{7'958}});
    ASSERT_NE(nearly_solid.surface_image, nullptr);
    auto const late_seam = animator.thermalSample(128U, 128U);
    ASSERT_TRUE(late_seam);
    EXPECT_GT(late_seam->progress, .98F);
    EXPECT_LT(late_seam->progress, 1.0F);
    auto solid = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{8'000}});
    auto paused = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed=std::chrono::milliseconds{8'300}});
    ASSERT_NE(hot.surface_image, nullptr);
    ASSERT_NE(solid.surface_image, nullptr);
    EXPECT_NE(hot.surface_image->rgba, solid.surface_image->rgba);
    // No independent incandescent seam survives the last cell's thermal sweep.
    EXPECT_EQ(solid.surface_image->rgba, paused.surface_image->rgba);
    EXPECT_EQ(solid.image->rgba, solid.surface_image->rgba);
}

client::GenerationMapAnimationFrame rainFrame(float precipitation, float wind, int64_t elapsed,
    std::chrono::milliseconds cooling_duration = client::GenerationMapAnimationClock{}.cooling_duration)
{
    auto snapshot = weatherSnapshot(true, false);
    std::fill(snapshot->v21->precipitation_mm_year.begin(), snapshot->v21->precipitation_mm_year.end(), precipitation);
    std::fill(snapshot->v21->phase_precipitation_mm_year.begin(),
        snapshot->v21->phase_precipitation_mm_year.end(), precipitation);
    std::fill(snapshot->v21->wind_u.begin(), snapshot->v21->wind_u.end(), wind);
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    return animator.updateNative(snapshot, nullptr, nullptr, {
        .visible_elapsed=std::chrono::milliseconds{elapsed}, .cooling_duration=cooling_duration});
}

TEST(GenerationHistoryRenderingTest, RainWaitsForConfiguredFullSolidificationAndPause)
{
    auto waiting = rainFrame(700.0F, 0.0F, 14'000, std::chrono::milliseconds{20'000});
    ASSERT_NE(waiting.image, nullptr);
    EXPECT_EQ(waiting.image->rgba, waiting.surface_image->rgba);
    auto raining = rainFrame(700.0F, 0.0F, 14'000);
    EXPECT_NE(raining.image->rgba, raining.surface_image->rgba);
}

std::vector<uint8_t> rainMask(float precipitation, float wind, int64_t elapsed)
{
    auto rainy = rainFrame(precipitation, wind, elapsed);
    auto dry = rainFrame(0.0F, wind, elapsed);
    EXPECT_EQ(rainy.surface_image->rgba, dry.surface_image->rgba);
    std::vector<uint8_t> mask(256U * 256U);
    for (uint32_t y = 0U; y < 256U; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            size_t const i = static_cast<size_t>(y * 2U) * rainy.image->width + x * 2U;
            mask[y * 256U + x] = rainy.image->rgba[i] != dry.image->rgba[i] ? 1U : 0U;
        }
    }
    return mask;
}

TEST(GenerationHistoryRenderingTest, RainUsesPrecipitationDensityWithoutRegularColumns)
{
    auto light = rainMask(175.0F, 0.0F, 14'000);
    auto heavy = rainMask(700.0F, 0.0F, 14'000);
    size_t light_count = 0U, heavy_count = 0U, occupied_columns = 0U;
    for (uint32_t x = 0U; x < 256U; ++x)
    {
        bool occupied = false;
        for (uint32_t y = 0U; y < 256U; ++y)
        {
            light_count += light[y * 256U + x];
            heavy_count += heavy[y * 256U + x];
            occupied = occupied || heavy[y * 256U + x] != 0U;
        }
        occupied_columns += occupied ? 1U : 0U;
    }
    EXPECT_GT(light_count, 0U);
    EXPECT_GT(heavy_count, light_count);
    EXPECT_GT(occupied_columns, 128U);
}

TEST(GenerationHistoryRenderingTest, RainImpactsStayAnchoredWhenAvailableWindChanges)
{
    auto east = rainMask(700.0F, 6.0F, 14'000);
    auto west = rainMask(700.0F, -6.0F, 14'000);
    ASSERT_GT(std::count(east.begin(), east.end(), uint8_t{1U}), 0);
    ASSERT_GT(std::count(west.begin(), west.end(), uint8_t{1U}), 0);
    EXPECT_EQ(east, west);
    auto later_east = rainMask(700.0F, 6.0F, 14'050);
    auto later_west = rainMask(700.0F, -6.0F, 14'050);
    ASSERT_GT(std::count(later_east.begin(), later_east.end(), uint8_t{1U}), 0);
    EXPECT_EQ(later_east, later_west);
    EXPECT_NE(east, later_east);
    EXPECT_EQ(east, rainMask(700.0F, 6.0F, 14'000));
}

TEST(GenerationHistoryRenderingTest, StaggeredRainMovesWithoutChangingTheScalarSurface)
{
    auto before = rainMask(700.0F, 0.0F, 13'950);
    auto after = rainMask(700.0F, 0.0F, 14'000);
    EXPECT_NE(before, after);
    auto same = rainMask(700.0F, 0.0F, 14'000);
    EXPECT_EQ(after, same);
}

template<typename T>
bool loadRainFixtureVector(std::filesystem::path const& file, std::vector<T>& values)
{
    std::error_code error;
    uintmax_t const bytes = std::filesystem::file_size(file, error);
    if (error || bytes % sizeof(T) != 0U || bytes > 64U * 1'024U * 1'024U)
    {
        return false;
    }
    values.resize(static_cast<size_t>(bytes / sizeof(T)));
    std::ifstream input(file, std::ios::binary);
    return static_cast<bool>(input.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(bytes)));
}

TEST(GenerationHistoryRenderingTest, InitialMoltenWorldHasCommonBrightIncandescence)
{
    auto snapshot = livePlatecSnapshot();
    client::GenerationMapAnimator animator;
    auto frame = animator.updateNative(snapshot, nullptr, nullptr, {});
    ASSERT_TRUE(frame.surface_image);
    uint8_t minimum_green = 255U, maximum_green = 0U;
    for (auto const& color : frame.surface_image->rgba)
    {
        EXPECT_GE(color[0U], 230U);
        minimum_green = std::min(minimum_green, color[1U]);
        maximum_green = std::max(maximum_green, color[1U]);
    }
    double minimum_block = 255.0, maximum_block = 0.0;
    for (uint32_t by = 0U; by < frame.surface_image->height; by += 64U)
    {
        for (uint32_t bx = 0U; bx < frame.surface_image->width; bx += 64U)
        {
            double mean = 0.0;
            for (uint32_t y = by; y < by + 64U; ++y)
            {
                for (uint32_t x = bx; x < bx + 64U; ++x)
                {
                    mean += frame.surface_image->rgba[y * frame.surface_image->width + x][1U];
                }
            }
            mean /= 64.0 * 64.0;
            minimum_block = std::min(minimum_block, mean);
            maximum_block = std::max(maximum_block, mean);
        }
    }
    // Rich fine material variation must not become large independent temperature clouds.
    EXPECT_GT(maximum_green, minimum_green);
    EXPECT_LT(maximum_block - minimum_block, .5 * (maximum_green - minimum_green));
}

TEST(GenerationHistoryRenderingTest, IncandescentCracksKeepFlickeringWhileGenerationWaits)
{
    auto snapshot = provisionalThermalSnapshot();
    client::GenerationMapAnimator animator;
    ASSERT_TRUE(animator.updateNative(snapshot, nullptr, nullptr, {}).surface_image);
    client::GenerationMapAnimationFrame before;
    client::GenerationMapAnimationFrame after;
    for (int64_t elapsed : {2'000, 4'000, 6'000, 120'000, 120'250})
    {
        auto frame = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        ASSERT_TRUE(frame.surface_image);
        if (char const* directory = std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR"))
        {
            writeImage(std::filesystem::path(directory) / ("wait-" + std::to_string(elapsed) + ".ppm"),
                *frame.surface_image);
        }
        if (elapsed == 120'000)
        {
            before = frame;
        }
        if (elapsed == 120'250)
        {
            after = frame;
        }
    }
    ASSERT_TRUE(before.surface_image);
    ASSERT_TRUE(after.surface_image);
    // A long source wait saturates provisional thermal age without extinguishing melt.
    // Its fissures must still visibly flicker, independently of further cooling or drift.
    EXPECT_FALSE(before.surface_image->rgba == after.surface_image->rgba);
    size_t incandescent = 0U;
    for (auto const& color : before.surface_image->rgba)
    {
        incandescent += color[0U] > 100U && color[0U] > color[1U] * 2U ? 1U : 0U;
    }
    EXPECT_GT(incandescent, 0U);
}

TEST(GenerationHistoryRenderingTest, HotMaterialBoilsSmoothlyAndSolidMaterialStopsFlickering)
{
    auto snapshot = provisionalThermalSnapshot();
    snapshot->stage = generation::GenerationStage::V21Terrain;
    client::GenerationMapAnimator animator;
    auto initial = animator.updateNative(snapshot, nullptr, nullptr, {});
    auto adjacent = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{42}});
    auto hot = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{250}});
    ASSERT_TRUE(initial.surface_image);
    ASSERT_TRUE(adjacent.surface_image);
    ASSERT_TRUE(hot.surface_image);
    EXPECT_NE(initial.surface_image->rgba, adjacent.surface_image->rgba);
    EXPECT_NE(initial.surface_image->rgba, hot.surface_image->rgba);
    for (size_t i = 0U; i < initial.surface_image->rgba.size(); ++i)
    {
        EXPECT_LT(std::abs(static_cast<int>(initial.surface_image->rgba[i][1U])
            - static_cast<int>(adjacent.surface_image->rgba[i][1U])), 20);
    }
    auto solid = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{8'000}});
    auto later = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{8'250}});
    ASSERT_TRUE(solid.surface_image);
    ASSERT_TRUE(later.surface_image);
    EXPECT_EQ(solid.surface_image->rgba, later.surface_image->rgba);
}

TEST(GenerationHistoryRenderingTest, WaterAndIceAdvanceAreSlowerWithoutDependingOnFrameRate)
{
    auto snapshot = weatherSnapshot(false, false);
    client::GenerationMapAnimator coarse, fine;
    ASSERT_TRUE(coarse.updateNative(snapshot, nullptr, nullptr, {}).surface_image);
    ASSERT_TRUE(fine.updateNative(snapshot, nullptr, nullptr, {}).surface_image);
    for (int64_t elapsed = 500; elapsed <= 42'000; elapsed += 500)
    {
        auto frame = fine.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        if (elapsed != 28'000 && elapsed != 37'000 && elapsed != 42'000)
        {
            continue;
        }
        auto jumped = coarse.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        EXPECT_EQ(frame.phase, jumped.phase);
        EXPECT_NEAR(frame.phase_progress, jumped.phase_progress, .0001F);
        if (elapsed == 28'000)
        {
            EXPECT_EQ(frame.phase, client::GenerationMapAnimationPhase::Water);
        }
        else
        {
            EXPECT_EQ(frame.phase, client::GenerationMapAnimationPhase::Ice);
            // Invasion occupies the first .10 of the .32-wide ice phase.
            if (elapsed == 37'000)
            {
                EXPECT_LT(frame.phase_progress, .10F / .32F);
            }
            else
            {
                EXPECT_GT(frame.phase_progress, .10F / .32F);
            }
        }
    }
}

TEST(GenerationHistoryRenderingTest, IceAgeDesertExpandsAcrossExposedVegetationAndReturnsToModernBiomes)
{
    auto snapshot = weatherSnapshot(false, false);
    auto& fields = *snapshot->v21;
    constexpr size_t COUNT = 256U * 256U;
    fields.source_elevation_m.assign(COUNT, 200.0F);
    fields.ocean_mask.assign(COUNT, 0U);
    fields.biome_id.assign(COUNT, 2U);
    fields.precipitation_mm_year.assign(COUNT, 700.0F);
    fields.phase_precipitation_mm_year.assign(COUNT * 8U, 700.0F);
    for (size_t i = 0U; i < COUNT; ++i)
    {
        float const temperature = 20.0F + 40.0F * static_cast<float>(i % 256U) / 256.0F;
        fields.temperature_celsius[i] = temperature;
        for (size_t phase = 0U; phase < 8U; ++phase)
        {
            fields.phase_temperature_celsius[phase * COUNT + i] = temperature;
        }
    }
    client::GenerationMapAnimator animator;
    ASSERT_TRUE(animator.updateNative(snapshot, nullptr, nullptr, {}).surface_image);
    auto growing = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{34'000}});
    auto peak = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{42'000}});
    auto modern = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{120'000}});
    auto endpoint = animator.updateNative(snapshot, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'800'000}});
    ASSERT_TRUE(growing.surface_image);
    ASSERT_TRUE(peak.surface_image);
    ASSERT_TRUE(modern.surface_image);
    ASSERT_TRUE(endpoint.surface_image);
    size_t const warm_index = 128U * 2U * peak.surface_image->width + 224U * 2U;
    auto const warm_growing = growing.surface_image->rgba[warm_index];
    auto const warm_peak = peak.surface_image->rgba[warm_index];
    auto const warm_modern = modern.surface_image->rgba[warm_index];
    EXPECT_GT(static_cast<int>(warm_peak[0U]) - warm_peak[1U],
        static_cast<int>(warm_growing[0U]) - warm_growing[1U]);
    EXPECT_GT(static_cast<int>(warm_peak[0U]) - warm_peak[1U],
        static_cast<int>(warm_modern[0U]) - warm_modern[1U]);
    EXPECT_EQ(modern.surface_image->rgba, endpoint.surface_image->rgba);
    if (char const* directory = std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR"))
    {
        writeImage(std::filesystem::path(directory) / "ice-growing.ppm", *growing.surface_image);
        writeImage(std::filesystem::path(directory) / "ice-peak.ppm", *peak.surface_image);
        writeImage(std::filesystem::path(directory) / "ice-modern.ppm", *modern.surface_image);
    }
}

TEST(GenerationHistoryRenderingTest, DISABLED_RetainedV10CoolingMaterialStrip)
{
    char const* input_directory = std::getenv("MC_RETAINED_V10_COOLING_DIR");
    char const* output_directory = std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR");
    ASSERT_NE(input_directory, nullptr); ASSERT_NE(output_directory, nullptr);
    std::filesystem::path const input(input_directory), output(output_directory);
    std::filesystem::create_directories(output);
    auto load = [&](std::string const& name, generation::GenerationSnapshot& snapshot) {
        auto& fields = snapshot.v10.emplace();
        if (!loadRainFixtureVector(input / (name + "-height_raw.bin"), fields.height_raw)
            || !loadRainFixtureVector(input / (name + "-source_elevation_m.bin"), fields.source_elevation_m)
            || !loadRainFixtureVector(input / (name + "-plate_ids.bin"), fields.plate_ids))
        {
            return false;
        }
        std::ifstream meta(input / (name + "-snapshot.txt"));
        std::string line;
        while (std::getline(meta, line))
        {
            auto const separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }
            auto const key = line.substr(0U, separator);
            auto const value = static_cast<uint32_t>(std::stoul(line.substr(separator + 1U)));
            if (key == "stage")
            {
                snapshot.stage = static_cast<generation::GenerationStage>(value);
            }
            if (key == "resolution")
            {
                snapshot.resolution = value;
            }
            if (key == "observed_native_step")
            {
                fields.plate_directions_observed_after_native_step = value;
            }
        }
        std::ifstream directions(input / (name + "-directions.csv"));
        std::getline(directions, line);
        while (std::getline(directions, line))
        {
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream row(line);
            generation::GenerationPlateDirection direction;
            if (!(row >> direction.plate_id >> direction.x >> direction.y))
            {
                return false;
            }
            fields.plate_directions.push_back(direction);
        }
        return generation::isValidGenerationSnapshot(snapshot);
    };
    auto initial = std::make_shared<generation::GenerationSnapshot>();
    auto terrain = std::make_shared<generation::GenerationSnapshot>();
    ASSERT_TRUE(load("initial", *initial)); ASSERT_TRUE(load("dry", *terrain));
    ASSERT_EQ(initial->stage, generation::GenerationStage::V10Platec);
    ASSERT_EQ(terrain->stage, generation::GenerationStage::V10Terrain);
    client::GenerationMapAnimator animator;
    std::ofstream metrics(output / "strip.csv");
    metrics << "elapsed_ms,potential_min,potential_max,progress_min,progress_max,solid_cells\n";
    // Deterministic replay uses the observed E8 H0 receive time, not a performance claim.
    std::vector<int64_t> times;
    for (int64_t elapsed = 0; elapsed <= 11'000; elapsed += 50)
    {
        times.push_back(elapsed);
    }
    times.insert(times.end(), {2'689, 2'731, 2'889, 3'589, 10'589, 10'639, 10'689});
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    std::vector<float> previous(256U * 256U, 0.0F);
    for (int64_t elapsed : times)
    {
        auto frame = animator.updateNative(elapsed < 2'689 ? initial : terrain, nullptr, nullptr,
            {.visible_elapsed = std::chrono::milliseconds{elapsed}});
        ASSERT_TRUE(frame.surface_image);
        float minimum_potential = 1.0F, maximum_potential = 0.0F;
        float minimum_progress = 1.0F, maximum_progress = 0.0F;
        uint32_t solid_cells = 0U;
        std::vector<float> values;
        values.reserve(256U * 256U * 3U);
        for (uint32_t y = 0U; y < 256U; ++y)
        {
            for (uint32_t x = 0U; x < 256U; ++x)
            {
                auto sample = animator.thermalSample(x, y);
                ASSERT_TRUE(sample);
                EXPECT_GE(sample->progress, previous[y * 256U + x]);
                previous[y * 256U + x] = sample->progress;
                minimum_potential = std::min(minimum_potential, sample->potential);
                maximum_potential = std::max(maximum_potential, sample->potential);
                minimum_progress = std::min(minimum_progress, sample->progress);
                maximum_progress = std::max(maximum_progress, sample->progress);
                solid_cells += sample->progress >= .74F ? 1U : 0U;
                values.insert(values.end(), {sample->elevation_m, sample->potential, sample->progress});
            }
        }
        metrics << elapsed << ',' << minimum_potential << ',' << maximum_potential << ','
            << minimum_progress << ',' << maximum_progress << ',' << solid_cells << '\n';
        if (elapsed % 250 == 0 || elapsed <= 1'500 || elapsed == 2'689 || elapsed >= 10'500)
        {
            writeImage(output / ("strip-" + std::to_string(elapsed) + ".ppm"), *frame.surface_image);
            std::ofstream raw(output / ("strip-" + std::to_string(elapsed) + ".f32"), std::ios::binary);
            raw.write(reinterpret_cast<char const*>(values.data()),
                static_cast<std::streamsize>(values.size() * sizeof(float)));
        }
    }
}

TEST(GenerationHistoryRenderingTest, DISABLED_ReplayAuthoritativeRainFixtureWithoutGeneration)
{
    char const* directory = std::getenv("MC_RAIN_AUTHORITATIVE_FIXTURE_DIR");
    ASSERT_NE(directory, nullptr);
    std::filesystem::path const input{directory};
    std::ifstream meta(input / "snapshot-meta.txt");
    std::string magic;
    uint32_t stage = 0U, resolution = 0U, native_step = 0U;
    ASSERT_TRUE(meta >> magic >> stage >> resolution >> native_step);
    ASSERT_EQ(magic, "MC_V21_SNAPSHOT_V1");
    ASSERT_EQ(stage, static_cast<uint32_t>(generation::GenerationStage::V21Water));
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = static_cast<generation::GenerationStage>(stage);
    snapshot->resolution = resolution;
    auto& fields = snapshot->v21.emplace();
    fields.plate_directions_observed_after_native_step = native_step;
#define LOAD_RAIN_FIELD(name) ASSERT_TRUE(loadRainFixtureVector(input / "snapshot-" #name ".bin", fields.name))
    LOAD_RAIN_FIELD(source_elevation_m);
    LOAD_RAIN_FIELD(temperature_celsius);
    LOAD_RAIN_FIELD(precipitation_mm_year);
    LOAD_RAIN_FIELD(wind_u);
    LOAD_RAIN_FIELD(wind_v);
    LOAD_RAIN_FIELD(current_u);
    LOAD_RAIN_FIELD(current_v);
    LOAD_RAIN_FIELD(phase_temperature_celsius);
    LOAD_RAIN_FIELD(phase_precipitation_mm_year);
    LOAD_RAIN_FIELD(land_snow_water_equivalent_m);
    LOAD_RAIN_FIELD(glacier_thickness_m);
    LOAD_RAIN_FIELD(sea_ice_thickness_m);
    LOAD_RAIN_FIELD(water_level_blocks);
    LOAD_RAIN_FIELD(ocean_mask);
    LOAD_RAIN_FIELD(lake_mask);
    LOAD_RAIN_FIELD(river_mask);
    LOAD_RAIN_FIELD(biome_id);
    LOAD_RAIN_FIELD(ice_surface_kind);
    LOAD_RAIN_FIELD(plate_ids);
#undef LOAD_RAIN_FIELD
    std::vector<uint32_t> ids;
    std::vector<float> directions;
    ASSERT_TRUE(loadRainFixtureVector(input / "snapshot-direction-ids.bin", ids));
    ASSERT_TRUE(loadRainFixtureVector(input / "snapshot-directions-xy.bin", directions));
    ASSERT_EQ(directions.size(), ids.size() * 2U);
    for (size_t i = 0U; i < ids.size(); ++i)
    {
        fields.plate_directions.push_back({ids[i], directions[i * 2U], directions[i * 2U + 1U]});
    }
    ASSERT_TRUE(generation::isValidGenerationSnapshot(*snapshot));
    client::GenerationMapAnimator animator;
    static_cast<void>(animator.updateNative(snapshot, nullptr, nullptr, {}));
    for (int64_t elapsed : {0, 2'000, 5'000, 10'000, 15'000, 20'000, 25'000, 30'000,
        35'000, 40'000, 45'000, 50'000, 60'000, 80'000, 1'800'000})
    {
        auto frame = animator.updateNative(snapshot, nullptr, nullptr,
            {.visible_elapsed=std::chrono::milliseconds{elapsed}, .ready=elapsed >= 1'800'000});
        ASSERT_NE(frame.image, nullptr);
        ASSERT_NE(frame.surface_image, nullptr);
        writeImage(input / ("replayed-" + std::to_string(elapsed) + ".ppm"), *frame.image);
    }
}

void recordV10ReplaySnapshot(std::filesystem::path const& output, std::string const& name,
    generation::GenerationSnapshot const& snapshot)
{
    ASSERT_TRUE(snapshot.v10.has_value());
    auto const& fields=*snapshot.v10;
    std::ofstream manifest(output/(name+"-fields.csv"));
    manifest<<"field,bytes,fnv64\n";
    auto save=[&](std::string const& field_name, auto const& values) {
        auto const bytes=std::as_bytes(std::span{values});
        uint64_t hash=14'695'981'039'346'656'037ULL;
        for(auto byte:bytes)
        {
            hash=(hash^static_cast<uint8_t>(byte))*1'099'511'628'211ULL;
        }
        std::ofstream raw(output/(name+"-"+field_name+".bin"),std::ios::binary);
        raw.write(reinterpret_cast<char const*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        manifest<<field_name<<','<<bytes.size()<<','<<hash<<'\n';
    };
#define SAVE_REPLAY_FIELD(field) save(#field,fields.field)
    SAVE_REPLAY_FIELD(height_raw);
    SAVE_REPLAY_FIELD(source_elevation_m);
    SAVE_REPLAY_FIELD(plate_ids);
    SAVE_REPLAY_FIELD(local_effective_celsius);
    SAVE_REPLAY_FIELD(ground_m);
    SAVE_REPLAY_FIELD(theta_celsius);
    SAVE_REPLAY_FIELD(phase_precipitation_mm_year);
    SAVE_REPLAY_FIELD(water_level_m);
    SAVE_REPLAY_FIELD(ocean_mask);
    SAVE_REPLAY_FIELD(lake_mask);
    SAVE_REPLAY_FIELD(river_mask);
    SAVE_REPLAY_FIELD(sea_ice_thickness_m);
    SAVE_REPLAY_FIELD(land_snow_water_equivalent_m);
    SAVE_REPLAY_FIELD(glacier_thickness_m);
#undef SAVE_REPLAY_FIELD
    std::ofstream directions(output/(name+"-directions.csv"));
    directions.precision(9);
    directions<<"plate_id,u,v\n";
    for(auto const& direction:fields.plate_directions)
    {
        directions<<direction.plate_id<<','<<direction.x<<','<<direction.y<<'\n';
    }
    std::ofstream meta(output/(name+"-snapshot.txt"));
    meta<<"stage="<<static_cast<unsigned>(snapshot.stage)<<"\nresolution="<<snapshot.resolution
        <<"\nobserved_native_step="<<fields.plate_directions_observed_after_native_step<<'\n';
}

void recordActualNativeStory(generation::TerrainModelTag model, bool real_field_wait_replay=false)
{
    char const* directory=std::getenv("MC_NATIVE_HISTORY_EVIDENCE_DIR");ASSERT_NE(directory,nullptr);
    std::filesystem::path output(directory);std::filesystem::create_directories(output);
    generation::GenerationConfiguration configuration;configuration.model_tag=model;configuration.seed=42U;
    client::GenerationMapAnimator streaming;
    std::shared_ptr<generation::GenerationSnapshot const> latest_snapshot, terrain_snapshot, water_snapshot, initial_snapshot;
    auto const started = std::chrono::steady_clock::now();
    uint32_t stream_frames = 0U;
    auto atlas=generation::prepareAtlas(configuration,{},[&](auto const& progress) {
        std::cout<<"[native-atlas] "<<static_cast<unsigned>(progress.stage)<<std::endl;
        if (!progress.snapshot) return;
        latest_snapshot = progress.snapshot;
        if(!initial_snapshot && latest_snapshot->v10)
        {
            initial_snapshot=latest_snapshot;
        }
        if (latest_snapshot->stage==generation::GenerationStage::V10Terrain)
        {
            terrain_snapshot=latest_snapshot;
        }
        if (latest_snapshot->stage==generation::GenerationStage::V10Water)
        {
            water_snapshot=latest_snapshot;
        }
        auto frame = streaming.updateNative(latest_snapshot, nullptr, nullptr,
            {.visible_elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-started), .ready=false});
        ASSERT_NE(frame.image, nullptr);
        if (stream_frames < 2U || (progress.stage != generation::GenerationStage::V21Platec
            && progress.stage != generation::GenerationStage::V10Platec))
            writeImage(output/("stream-"+std::to_string(stream_frames)+".ppm"), *frame.image);
        ++stream_frames;
    });
    ASSERT_NE(atlas,nullptr);uint64_t fingerprint=14'695'981'039'346'656'037ULL;
    auto const registered=model==generation::TerrainModelTag::V10
        ? generation::v10RegisteredFields(*atlas) : model==generation::TerrainModelTag::V21
        ? generation::v21RegisteredFields(*atlas) : generation::v9RegisteredFields(*atlas);
    std::ofstream field_manifest(output/"fields.tsv");
    field_manifest<<"name,bytes,fnv64,file\n";
    size_t field_index=0U;
    for(auto const& field:registered)
    {
        std::visit([&](auto values) {
            auto const bytes=std::as_bytes(values);
            uint64_t field_hash=14'695'981'039'346'656'037ULL;
            for(auto byte:bytes)
            {
                fingerprint=(fingerprint^static_cast<uint8_t>(byte))*1'099'511'628'211ULL;
                field_hash=(field_hash^static_cast<uint8_t>(byte))*1'099'511'628'211ULL;
            }
            auto const filename="field-"+std::to_string(field_index++)+".bin";
            std::ofstream raw(output/filename,std::ios::binary);
            raw.write(reinterpret_cast<char const*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            field_manifest<<field.name<<','<<bytes.size()<<','<<field_hash<<','<<filename<<'\n';
        },field.values);
    }
    shared::TerrainWorld world{atlas};auto lod=shared::buildTerrainLod(world,shared::TerrainLodLevel::L0);ASSERT_NE(lod,nullptr);
    auto map=client::rasterizeS2WorldMap(world,*lod,{});ASSERT_TRUE(map);
    auto source=std::make_shared<client::S2MapImage const>(std::move(*map));
    std::optional<generation::V10GenerationSnapshotFields> physical_v10_before;
    if (model == generation::TerrainModelTag::V10)
    {
        ASSERT_TRUE(latest_snapshot);
        ASSERT_TRUE(latest_snapshot->v10);
        physical_v10_before = *latest_snapshot->v10;
        ASSERT_EQ(source->v10_projected_inland_ice_kind.size(), 256U * 256U);
        EXPECT_EQ(source->v10_projected_inland_ice_kind[194U], client::S2_MAP_PROJECTED_INLAND_ICE);
        EXPECT_EQ(source->v10_ice_surface_kind[194U], 0U);
        EXPECT_EQ(source->v10_projected_inland_ice_kind[48U * 256U + 173U], 0U);
        EXPECT_EQ(source->v10_projected_inland_ice_kind[48U * 256U + 189U], 0U);
    }
    {
        std::ofstream identity(output/"identity.txt");
        identity<<"model="<<static_cast<unsigned>(model)<<"\nseed="<<configuration.seed
            <<"\nsource_width="<<source->width<<"\nsource_height="<<source->height
            <<"\nphysical_fields_fnv64="<<fingerprint<<'\n';
        std::ofstream raw(output/"canonical-source.rgba",std::ios::binary);
        raw.write(reinterpret_cast<char const*>(source->rgba.data()),
            static_cast<std::streamsize>(source->rgba.size()*sizeof(client::GenerationMapRgba8)));
    }
    if (real_field_wait_replay)
    {
        ASSERT_EQ(model,generation::TerrainModelTag::V10);
        ASSERT_NE(terrain_snapshot,nullptr); ASSERT_NE(water_snapshot,nullptr);
        ASSERT_TRUE(generation::isValidGenerationSnapshot(*terrain_snapshot));
        ASSERT_TRUE(generation::isValidGenerationSnapshot(*water_snapshot));
        ASSERT_EQ(terrain_snapshot->stage,generation::GenerationStage::V10Terrain);
        ASSERT_EQ(water_snapshot->stage,generation::GenerationStage::V10Water);
        recordV10ReplaySnapshot(output,"dry",*terrain_snapshot);
        recordV10ReplaySnapshot(output,"hydrology",*water_snapshot);
        std::ofstream replay_identity(output/"real-field-replay-identity.txt");
        replay_identity<<"Deterministic retained-real-field clock replay; not Windows/runtime/performance evidence.\n"
            <<"configuration="<<generation::configurationIdentity(configuration)<<'\n';
        ASSERT_TRUE(initial_snapshot);
        recordV10ReplaySnapshot(output,"initial",*initial_snapshot);
        client::GenerationMapAnimator thermal_replay;
        ASSERT_TRUE(thermal_replay.updateNative(initial_snapshot,nullptr,nullptr,{}).surface_image);
        std::ofstream thermal_csv(output/"real-thermal-witness.csv");
        thermal_csv<<"elapsed_ms,x,y,height_m,potential,progress\n";
        float previous_summit=0.0F,previous_lowland=0.0F;
        int64_t const cooling_budget_ms = client::GenerationMapAnimationClock{}.cooling_duration.count();
        int64_t const thermal_completion_ms = 1'000 + cooling_budget_ms;
        std::vector<int64_t> const thermal_times{1'000,1'042,1'200,1'900,2'000,3'000,4'000,5'000,6'000,
            6'200,6'400,6'600,6'800,6'900,6'958,6'990,7'000,8'000,
            thermal_completion_ms - 42, thermal_completion_ms, thermal_completion_ms + 42};
        for(int64_t elapsed : thermal_times)
        {
            auto const snapshot=elapsed<1'000 ? initial_snapshot : terrain_snapshot;
            auto thermal_frame=thermal_replay.updateNative(snapshot,nullptr,nullptr,
                {.visible_elapsed=std::chrono::milliseconds{elapsed}});
            ASSERT_TRUE(thermal_frame.surface_image);
            writeImage(output/("real-thermal-"+std::to_string(elapsed)+".ppm"),*thermal_frame.surface_image);
            std::vector<float> values;
            values.reserve(256U*256U*3U);
            for(uint32_t y=0U;y<256U;++y)
            {
                for(uint32_t x=0U;x<256U;++x)
                {
                    auto sample=thermal_replay.thermalSample(x,y);
                    ASSERT_TRUE(sample);
                    values.insert(values.end(),{sample->elevation_m,sample->potential,sample->progress});
                }
            }
            std::ofstream raw(output/("real-thermal-"+std::to_string(elapsed)+".f32"),std::ios::binary);
            raw.write(reinterpret_cast<char const*>(values.data()),static_cast<std::streamsize>(values.size()*sizeof(float)));
            auto summit=thermal_replay.thermalSample(176U,16U),lowland=thermal_replay.thermalSample(141U,37U);
            ASSERT_TRUE(summit); ASSERT_TRUE(lowland);
            EXPECT_GE(summit->progress,previous_summit); EXPECT_GE(lowland->progress,previous_lowland);
            previous_summit=summit->progress; previous_lowland=lowland->progress;
            thermal_csv<<elapsed<<",176,16,"<<summit->elevation_m<<','<<summit->potential<<','<<summit->progress<<'\n';
            thermal_csv<<elapsed<<",141,37,"<<lowland->elevation_m<<','<<lowland->potential<<','<<lowland->progress<<'\n';
            if(elapsed==1'000)
            {
                EXPECT_FLOAT_EQ(summit->progress,0.0F); EXPECT_FLOAT_EQ(lowland->progress,0.0F);
            }
            if(elapsed==1'900)
            {
                EXPECT_GT(summit->potential,lowland->potential);
                EXPECT_GT(summit->progress,lowland->progress);
            }
            if (elapsed == thermal_completion_ms - 42)
            {
                EXPECT_LT(lowland->progress, 1.0F);
            }
            if (elapsed >= thermal_completion_ms)
            {
                EXPECT_FLOAT_EQ(summit->progress,1.0F); EXPECT_FLOAT_EQ(lowland->progress,1.0F);
            }
        }
        client::GenerationMapAnimator replay;
        ASSERT_TRUE(replay.updateNative(terrain_snapshot,nullptr,nullptr,{}).surface_image);
        client::GenerationMapAnimationFrame dry_frame;
        for(int64_t elapsed:{9'000,10'000,12'000,14'000})
        {
            dry_frame=replay.updateNative(terrain_snapshot,nullptr,nullptr,
                {.visible_elapsed=std::chrono::milliseconds{elapsed}});
            ASSERT_TRUE(dry_frame.surface_image);
            writeImage(output/("real-dry-"+std::to_string(elapsed)+".ppm"),*dry_frame.surface_image);
        }
        auto adoption=replay.updateNative(water_snapshot,nullptr,nullptr,
            {.visible_elapsed=std::chrono::milliseconds{14'000}});
        ASSERT_TRUE(adoption.surface_image);
        EXPECT_EQ(adoption.surface_image->rgba,dry_frame.surface_image->rgba);
        writeImage(output/"real-adopt-0.ppm",*adoption.surface_image);
        for(int64_t delta:{42,200,2'000})
        {
            auto frame=replay.updateNative(water_snapshot,nullptr,nullptr,
                {.visible_elapsed=std::chrono::milliseconds{14'000+delta}});
            ASSERT_TRUE(frame.surface_image);
            writeImage(output/("real-adopt-"+std::to_string(delta)+".ppm"),*frame.surface_image);
        }
        for(int64_t arrival_ms:{10'179,6'815})
        {
            std::string const label=arrival_ms==10'179 ? "cold" : "warm";
            client::GenerationMapAnimator ordinary;
            ASSERT_TRUE(ordinary.updateNative(initial_snapshot,nullptr,nullptr,{}).surface_image);
            std::vector<int64_t> times;
            for(int64_t elapsed=500;elapsed<arrival_ms;elapsed+=500)
            {
                times.push_back(elapsed);
            }
            std::vector<int64_t> const arrival_deltas{0,42,200,900,2'000,4'000,6'000,7'000,7'350,
                cooling_budget_ms - 42, cooling_budget_ms, cooling_budget_ms + 350};
            for(int64_t delta : arrival_deltas)
            {
                times.push_back(arrival_ms+delta);
            }
            std::ofstream ordinary_csv(output/("ordinary-"+label+"-thermal.csv"));
            ordinary_csv<<"elapsed_ms,height_m,potential,progress\n";
            float last_progress=0.0F;
            for(int64_t elapsed:times)
            {
                auto const observation=elapsed<arrival_ms ? initial_snapshot
                    : elapsed<=arrival_ms+900 ? terrain_snapshot : water_snapshot;
                auto frame=ordinary.updateNative(observation,nullptr,nullptr,
                    {.visible_elapsed=std::chrono::milliseconds{elapsed}});
                ASSERT_TRUE(frame.surface_image);
                auto sample=ordinary.thermalSample(176U,16U);
                ASSERT_TRUE(sample);
                EXPECT_GE(sample->progress,last_progress);
                if(elapsed<arrival_ms)
                {
                    EXPECT_LT(sample->progress,1.0F);
                }
                if (elapsed == arrival_ms + 7'000)
                {
                    EXPECT_LT(sample->progress, 1.0F);
                }
                if (elapsed >= arrival_ms + cooling_budget_ms)
                {
                    EXPECT_FLOAT_EQ(sample->progress,1.0F);
                }
                ordinary_csv<<elapsed<<','<<sample->elevation_m<<','<<sample->potential<<','<<sample->progress<<'\n';
                writeImage(output/("ordinary-"+label+"-"+std::to_string(elapsed)+".ppm"),*frame.surface_image);
                last_progress=sample->progress;
            }
        }
        client::GenerationMapAnimator ice_replay;
        ASSERT_TRUE(ice_replay.updateNative(atlas,source,{}).surface_image);
        for(int64_t elapsed:{16'000,18'000,19'040,21'280,22'500,24'000,25'200,28'000})
        {
            auto ice_frame=ice_replay.updateNative(atlas,source,
                {.visible_elapsed=std::chrono::milliseconds{elapsed}});
            ASSERT_TRUE(ice_frame.surface_image);
            writeImage(output/("real-ice-"+std::to_string(elapsed)+".ppm"),*ice_frame.surface_image);
        }
        auto final_adoption=replay.updateNative(water_snapshot,atlas,source,
            {.visible_elapsed=std::chrono::milliseconds{16'000}});
        ASSERT_TRUE(final_adoption.surface_image);
        auto endpoint=replay.updateNative(water_snapshot,atlas,source,
            {.visible_elapsed=std::chrono::milliseconds{60'000}});
        ASSERT_TRUE(endpoint.surface_image);
        EXPECT_EQ(endpoint.surface_image->rgba,source->rgba);
        auto const endpoint_thermal = replay.thermalSample(176U, 16U);
        ASSERT_TRUE(endpoint_thermal);
        EXPECT_FLOAT_EQ(endpoint_thermal->progress, 1.0F);
        if (physical_v10_before)
        {
            EXPECT_EQ(*latest_snapshot->v10, *physical_v10_before);
        }
        writeImage(output/"real-endpoint.ppm",*endpoint.surface_image);
        writeImage(output/"real-canonical.ppm",client::GenerationMapImage{source->width,source->height,source->rgba});
        return;
    }
    std::cout<<"[native-source] stock snow="<<source->ice_snow_count
        <<" glacier="<<source->ice_glacier_count<<" sea="<<source->ice_water_count<<std::endl;
    for(auto const& field:registered)
    {
        if(field.name!="temperature_C")
        {
            continue;
        }
        std::visit([&](auto values) {
            std::vector<float> converted;
            for(auto value:values)
            {
                converted.push_back(static_cast<float>(value));
            }
            std::ofstream raw(output/"temperature.f32",std::ios::binary);
            raw.write(reinterpret_cast<char const*>(converted.data()),
                static_cast<std::streamsize>(converted.size()*sizeof(float)));
        },field.values);
    }
    auto const& stock_kind=model==generation::TerrainModelTag::V10?source->v10_ice_surface_kind
        :model==generation::TerrainModelTag::V21?source->v21_ice_surface_kind:source->v9_ice_surface_kind;
    {std::ofstream raw(output/"stock-kind.u8",std::ios::binary);
    raw.write(reinterpret_cast<char const*>(stock_kind.data()),static_cast<std::streamsize>(stock_kind.size()));
    }
    ASSERT_GT(stream_frames, 1U);
    auto const final_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started);
    auto const before_atlas = streaming.updateNative(latest_snapshot, nullptr, nullptr,
        {.visible_elapsed=final_elapsed, .ready=false});
    auto const with_atlas = streaming.updateNative(latest_snapshot, atlas, source,
        {.visible_elapsed=final_elapsed, .ready=true});
    ASSERT_NE(before_atlas.image, nullptr); ASSERT_NE(with_atlas.image, nullptr);
    EXPECT_EQ(before_atlas.phase, with_atlas.phase);
    EXPECT_EQ(before_atlas.phase_progress, with_atlas.phase_progress);
    EXPECT_EQ(before_atlas.image->rgba, with_atlas.image->rgba);
    auto const exact = streaming.updateNative(latest_snapshot, atlas, source,
        {.visible_elapsed=final_elapsed+std::chrono::seconds{30}, .ready=true});
    ASSERT_NE(exact.surface_image, nullptr);
    EXPECT_EQ(exact.surface_image->rgba, source->rgba);
    auto const exact_thermal = streaming.thermalSample(176U, 16U);
    ASSERT_TRUE(exact_thermal);
    EXPECT_FLOAT_EQ(exact_thermal->progress, 1.0F);
    if (physical_v10_before)
    {
        EXPECT_EQ(*latest_snapshot->v10, *physical_v10_before);
    }
    client::GenerationMapImage expected{source->width,source->height,source->rgba};
    writeImage(output/"canonical-final.ppm",expected);
    client::GenerationMapAnimator arrival;
    auto prelude=arrival.updateNative(nullptr,nullptr,{.visible_elapsed=std::chrono::milliseconds{5000}});
    auto arriving=arrival.updateNative(atlas,source,{.visible_elapsed=std::chrono::milliseconds{5000}});
    EXPECT_EQ(prelude.image,nullptr);ASSERT_NE(arriving.image,nullptr);
    client::GenerationMapAnimator reduced;
    static_cast<void>(reduced.updateNative(atlas,source,{}));
    auto skipped=reduced.updateNative(atlas,source,{.reduced_motion=true});
    ASSERT_NE(skipped.surface_image,nullptr);EXPECT_EQ(skipped.surface_image->rgba,source->rgba);
    auto const skipped_thermal = reduced.thermalSample(176U, 16U);
    ASSERT_TRUE(skipped_thermal);
    EXPECT_FLOAT_EQ(skipped_thermal->progress, 1.0F);
    auto const& final_heights = model == generation::TerrainModelTag::V10 ? source->v10_elevation_m
        : model == generation::TerrainModelTag::V21 ? source->v21_elevation_m : source->v9_elevation_m;
    EXPECT_FLOAT_EQ(skipped_thermal->elevation_m, static_cast<float>(final_heights[16U * 256U + 176U]));
    auto resumed=reduced.updateNative(atlas,source,{});
    EXPECT_EQ(resumed.phase,client::GenerationMapAnimationPhase::Arrival);
    EXPECT_EQ(resumed.surface_image->rgba,source->rgba);
    auto mismatched=std::make_shared<client::S2MapImage>(*source);++mismatched->source_identity.seed;
    client::GenerationMapAnimator mismatch;
    auto rejected=mismatch.updateNative(atlas,mismatched,{});
    EXPECT_EQ(rejected.image, nullptr);
    EXPECT_EQ(rejected.surface_image, nullptr);
    client::GenerationMapAnimator animator;std::vector<int64_t> times;
    for(int64_t t:{0,1'000,2'000,3'000,4'000,5'000,6'000,7'000,7'300,7'350,8'000,9'000,
        12'000,14'000,16'000,19'040,21'280,22'500,24'000,25'200,28'000,29'400})times.push_back(t);
    for(int64_t boundary:{3920,8400,16'240,22'960,28'000})for(int64_t epsilon:{-42,-1,0,1,42})times.push_back(boundary+epsilon);
    std::sort(times.begin(),times.end());times.erase(std::unique(times.begin(),times.end()),times.end());
    std::ofstream cost(output/"cost.csv");cost<<"source_fnv64,"<<fingerprint<<"\nelapsed_ms,wall_us,cpu_us,phase\n";
    for(int64_t t:times)
    {
        auto start=std::chrono::steady_clock::now();auto cpu=std::clock();
        auto frame=animator.updateNative(atlas,source,{
            .visible_elapsed=std::chrono::milliseconds{t},.formation_duration=std::chrono::seconds{10}});
        auto cpu_us=static_cast<int64_t>(static_cast<double>(std::clock()-cpu)*1'000'000.0/CLOCKS_PER_SEC);
        auto wall=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        ASSERT_NE(frame.image,nullptr);ASSERT_NE(frame.surface_image,nullptr);
        writeImage(output/("story-"+std::to_string(t)+".ppm"),*frame.image);
        writeImage(output/("surface-"+std::to_string(t)+".ppm"),*frame.surface_image);
        if(t>=28'000)EXPECT_EQ(frame.surface_image->rgba,source->rgba);
        cost<<t<<','<<wall<<','<<cpu_us<<','<<static_cast<unsigned>(frame.phase)<<'\n';
        if(t%3500==0||t==28'000)std::cout<<"[native-story] elapsed="<<t<<" wall_us="<<wall<<std::endl;
    }
}
TEST(GenerationHistoryRenderingTest, DISABLED_ActualV10RetainedFieldWaitReplay)
{
    recordActualNativeStory(generation::TerrainModelTag::V10,true);
}
TEST(GenerationHistoryRenderingTest, DISABLED_ActualV10TypedHistoryCpuStory)
{
    recordActualNativeStory(generation::TerrainModelTag::V10);
}
TEST(GenerationHistoryRenderingTest, DISABLED_ActualV21TypedHistoryCpuStory)
{
    recordActualNativeStory(generation::TerrainModelTag::V21);
}
TEST(GenerationHistoryRenderingTest, DISABLED_ActualV9TypedHistoryCpuStory)
{
    recordActualNativeStory(generation::TerrainModelTag::V9);
}
} // namespace
