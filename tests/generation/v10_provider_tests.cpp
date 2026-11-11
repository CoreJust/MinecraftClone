#include "V10ProviderInternal.hpp"

#if defined(MC_TEST_HAS_CPU_MAP)
#include <client/GenerationMapPresentation.hpp>
#include <client/S2MapPresentation.hpp>
#include <server/WorldPreparation.hpp>
#endif
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/RiverBedChannel.hpp>
#include <generation/V10Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using namespace generation;

GenerationConfiguration configuration()
{
    GenerationConfiguration result;
    result.model_tag = TerrainModelTag::V10;
    return result;
}

#if defined(MC_TEST_HAS_CPU_MAP)
uint64_t profileMapChecksum(client::S2MapImage const& image)
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    auto const add_bytes = [&hash](std::span<std::byte const> bytes) {
        for (std::byte const byte : bytes)
        {
            hash ^= std::to_integer<uint8_t>(byte);
            hash *= 1'099'511'628'211ULL;
        }
    };
    auto const add_scalar = [&add_bytes](auto const& value) {
        add_bytes(std::as_bytes(std::span{&value, 1U}));
    };
    auto const add_range = [&add_bytes](auto const& values) {
        add_bytes(std::as_bytes(std::span{values}));
    };

    add_scalar(image.width);
    add_scalar(image.height);
    add_scalar(image.source_identity.terrain_version);
    add_scalar(image.source_identity.revision);
    add_scalar(image.source_identity.seed);
    add_range(image.source_identity.configuration_identity);
    add_range(image.source_model_identity);
    add_range(image.available_layers);
    add_range(image.layer_ranges);
    add_range(image.has_numeric_range);
    add_scalar(image.ice_snow_count);
    add_scalar(image.ice_glacier_count);
    add_scalar(image.ice_water_count);
    add_scalar(image.has_display_ice_mask);
    add_scalar(image.source_resolution);
    add_scalar(image.vector_field_resolution);
    add_scalar(image.ocean_mask_resolution);
    add_range(image.v10_river_base_rgba);
    add_range(image.v10_river_coverage);
    add_range(image.v10_elevation_m);
    add_range(image.v10_temperature_mean_c);
    add_range(image.v10_precipitation_mean_mm_year);
    add_range(image.v10_biome_ids);
    add_range(image.v10_moisture_mean_kg_m2);
    add_range(image.v10_wind_u_m_s);
    add_range(image.v10_wind_v_m_s);
    add_range(image.v10_ocean_u_m_s);
    add_range(image.v10_ocean_v_m_s);
    add_range(image.v10_ocean_cells);
    add_range(image.v10_ice_surface_kind);
    add_range(image.v10_projected_inland_ice_kind);
    add_range(image.rgba);
    return hash;
}

std::string profileMapFlags(client::S2MapImage const& image)
{
    std::string result;
    result.reserve(image.available_layers.size() * 2U);
    for (size_t index = 0U; index < image.available_layers.size(); ++index)
    {
        result.push_back(image.available_layers[index] ? '1' : '0');
        result.push_back(image.has_numeric_range[index] ? '1' : '0');
    }
    return result;
}

#endif

size_t exportFineTerrainDiagnosticGrids(GlobalAtlas const& atlas)
{
    char const* directory_value = std::getenv("MC_V10_FINE_DIAGNOSTIC_DIR");
    if (directory_value == nullptr || *directory_value == '\0')
    {
        return 0U;
    }

    struct Site final
    {
        std::string_view name;
        double x_m;
        double y_m;
    };
    constexpr std::array<Site, 5U> SITES{{
        {"ridge", 52'372.486'699'750'08, 4'937.492'867'001'128},
        {"s2-problem", 35'360.0, 33'230.0},
        {"signed-coast", 45'056.0, 0.0},
        {"plain", 25'856.0, 9'728.0},
        {"river-channel", 34'784.0, 30'212.0},
    }};
    constexpr std::array<double, 2U> PITCHES_M{2.0, 8.0};
    constexpr size_t GRID_SIDE = 128U;
    std::filesystem::path const directory{directory_value};
    std::filesystem::create_directories(directory);
    size_t written = 0U;
    for (Site const& site : SITES)
    {
        for (double const pitch_m : PITCHES_M)
        {
            std::string const filename = std::string{site.name} + "-p"
                + std::to_string(static_cast<uint32_t>(pitch_m)) + "m.csv";
            std::filesystem::path const file = directory / filename;
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error("cannot create V10 fine diagnostic grid");
            }
            output << "row,column,x_m,y_m,canonical_m,l2_m,l3_m,canonical_river,l2_river,l3_river,"
                "canonical_lake,l2_lake,l3_lake,canonical_water_m,l2_water_m,l3_water_m\n";
            output << std::setprecision(17);
            for (size_t row = 0U; row < GRID_SIDE; ++row)
            {
                double const y_offset = (static_cast<double>(row) - (GRID_SIDE - 1U) * 0.5) * pitch_m;
                double const y_m = site.y_m + y_offset;
                for (size_t column = 0U; column < GRID_SIDE; ++column)
                {
                    double const x_offset = (static_cast<double>(column) - (GRID_SIDE - 1U) * 0.5) * pitch_m;
                    double const x_m = site.x_m + x_offset;
                    auto const canonical = sampleV10Surface(atlas, x_m, y_m, 3U);
                    auto const l2 = sampleV10FineTerrainSurface(atlas, x_m, y_m, 2U);
                    auto const l3 = sampleV10FineTerrainSurface(atlas, x_m, y_m, 3U);
                    output << row << ',' << column << ',' << x_m << ',' << y_m << ','
                        << canonical.elevation_m << ',' << l2.elevation_m << ',' << l3.elevation_m << ','
                        << canonical.river << ',' << l2.river << ',' << l3.river << ','
                        << canonical.lake << ',' << l2.lake << ',' << l3.lake << ','
                        << canonical.water_level_m << ',' << l2.water_level_m << ',' << l3.water_level_m << '\n';
                }
            }
            output.flush();
            if (!output)
            {
                throw std::runtime_error("failed writing V10 fine diagnostic grid");
            }
            ++written;
        }
    }
    return written;
}

TEST(V10ProviderTests, ModelIdentityAndNumericTagsAreDistinct)
{
    static_assert(static_cast<uint8_t>(TerrainModelTag::R6) == 0U);
    static_assert(static_cast<uint8_t>(TerrainModelTag::V9) == 1U);
    static_assert(static_cast<uint8_t>(TerrainModelTag::V21) == 2U);
    static_assert(static_cast<uint8_t>(GenerationStage::Complete) == 15U);
    EXPECT_EQ(terrainVersion(TerrainModelTag::V10), 10U);
    auto config = configuration();
    auto const identity = configurationIdentity(config);
    EXPECT_LE(identity.size(), CONFIGURATION_IDENTITY_MAX_BYTES);
    EXPECT_NE(identity.find("f775213330028b165328885c6899ac0bf9c3c0c9a93ff6e0aca591554dec6e59"), std::string::npos);
    EXPECT_NE(identity.find("scale=1;datum=512;depth=2048"), std::string::npos);
    EXPECT_NE(identity.find("river=c8f9b851f7891e555165648d473b39df1ecffc9b73ae65cf5b14ed9f5ba04703"), std::string::npos);
    config.model_tag = TerrainModelTag::V21;
    EXPECT_NE(identity, configurationIdentity(config));
}

TEST(V10ProviderTests, AnnualMeanSurfaceCoverSharesColdSnowAndWaterRules)
{
    V10SurfaceSample sample;
    sample.temperature_celsius = -8.846;
    sample.snow_water_equivalent_m = 0.018'859;
    sample.glacier_thickness_m = 0.005'096;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::Snow);

    sample.temperature_celsius = 0.0;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::None);

    sample.snow_water_equivalent_m = 0.1;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::Snow);

    sample.temperature_celsius = 5.0;
    sample.snow_water_equivalent_m = 0.0;
    sample.glacier_thickness_m = 0.1;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::GlacierIce);

    sample.temperature_celsius = -8.846;
    sample.snow_water_equivalent_m = 0.018'859;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::Snow);

    sample.lake = true;
    sample.snow_water_equivalent_m = 0.5;
    sample.glacier_thickness_m = 0.5;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::None);
    sample.lake = false;
    sample.river = true;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::None);

    sample.river = false;
    sample.ocean = true;
    sample.sea_ice_thickness_m = 0.099;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::None);
    sample.sea_ice_thickness_m = 0.1;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::SeaIce);

    sample.ocean = false;
    sample.sea_ice_thickness_m = 0.0;
    sample.snow_water_equivalent_m = 0.0;
    sample.glacier_thickness_m = 0.0;
    sample.temperature_celsius = -5.0;
    EXPECT_EQ(classifyV10AnnualMeanSurfaceCover(sample), V10SurfaceCover::None);
}

TEST(V10ProviderTests, RejectsUnsupportedSeed)
{
    auto config = configuration();
    config.seed = 43U;
    EXPECT_THROW((void)configurationIdentity(config), std::invalid_argument);
    EXPECT_THROW((void)prepareAtlas(config), std::invalid_argument);
}

TEST(V10ProviderTests, RejectsUnsupportedResolution)
{
    auto config = configuration();
    config.resolution = 512U;
    EXPECT_THROW((void)configurationIdentity(config), std::invalid_argument);
    EXPECT_THROW((void)prepareAtlas(config), std::invalid_argument);
}

TEST(V10ProviderTests, CancellationDoesNotPublishAnAtlas)
{
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW((void)prepareAtlas(configuration(), stop.get_token()), GenerationCancelled);
}

TEST(V10ProviderTests, OneMetreMappingRejectsOutOfDomainInsteadOfClamping)
{
    EXPECT_EQ(mapV10ElevationToBlock(723.342'269'667'154'6), 1235U);
    EXPECT_EQ(mapV10ElevationToBlock(-500.0), 12U);
    EXPECT_EQ(mapV10ElevationToBlock(-512.0), 0U);
    EXPECT_EQ(mapV10ElevationToBlock(1535.9), 2047U);
    EXPECT_THROW((void)mapV10ElevationToBlock(-512.01), std::out_of_range);
    EXPECT_THROW((void)mapV10ElevationToBlock(1536.0), std::out_of_range);
    EXPECT_THROW((void)mapV10ElevationToBlock(std::numeric_limits<double>::quiet_NaN()), std::out_of_range);
}

TEST(V10ProviderTests, WaterGeometryMatchesEverySurfacePhaseAtMapContacts)
{
    auto config = configuration();
    config.seed = 42U;
    auto const atlas = prepareAtlas(config);
    ASSERT_NE(atlas, nullptr);

    auto const expect_matches = [&](GlobalAtlas const& source, double const x, double const y, uint32_t const lod) {
        auto const geometry = sampleV10WaterGeometry(source, x, y, lod);
        auto const phases = sampleV10SurfacePhases(source, x, y, lod);
        for (size_t phase = 0U; phase < phases.size(); ++phase)
        {
            EXPECT_DOUBLE_EQ(geometry.elevation_m, phases[phase].elevation_m) << "phase=" << phase;
            EXPECT_DOUBLE_EQ(geometry.water_level_m, phases[phase].water_level_m) << "phase=" << phase;
            EXPECT_EQ(geometry.ocean, phases[phase].ocean) << "phase=" << phase;
            EXPECT_EQ(geometry.lake, phases[phase].lake) << "phase=" << phase;
            EXPECT_EQ(geometry.river, phases[phase].river) << "phase=" << phase;
        }
        return geometry;
    };

    auto const dry_land = expect_matches(*atlas, 52'372.486'699'750'08, 4'937.492'867'001'128, 0U);
    EXPECT_FALSE(dry_land.ocean || dry_land.lake || dry_land.river);

    auto const ocean = expect_matches(*atlas, 34'860.0, 30'208.0, 0U);
    EXPECT_TRUE(ocean.ocean);

    auto const lake = expect_matches(*atlas, 43'520.0, 9'216.0, 0U);
    EXPECT_TRUE(lake.lake);

    auto const river = expect_matches(*atlas, 34'816.0, 30'208.0, 0U);
    EXPECT_TRUE(river.river);
    for (uint32_t lod = 1U; lod <= 3U; ++lod)
    {
        auto const lod_river = expect_matches(*atlas, 34'816.0, 30'208.0, lod);
        EXPECT_EQ(lod_river.river, river.river);
    }

    auto const polar_contact = expect_matches(*atlas, 185.0 * 256.0, 12.0 * 256.0, 0U);
    EXPECT_FALSE(polar_contact.ocean);
    EXPECT_TRUE(polar_contact.lake || polar_contact.river);

    auto const wet_boundary_side = expect_matches(*atlas, 34'854.0, 30'204.0, 0U);
    EXPECT_TRUE(wet_boundary_side.ocean);
    auto const dry_boundary_side = expect_matches(*atlas, 34'860.0, 30'200.0, 0U);
    EXPECT_FALSE(dry_boundary_side.ocean || dry_boundary_side.lake || dry_boundary_side.river);

    auto const periodic_river = expect_matches(*atlas, 34'816.0 - 65'536.0, 30'208.0 + 65'536.0, 0U);
    EXPECT_EQ(periodic_river.river, river.river);
    EXPECT_DOUBLE_EQ(periodic_river.elevation_m, river.elevation_m);
    EXPECT_DOUBLE_EQ(periodic_river.water_level_m, river.water_level_m);

    auto fine_config = config;
    fine_config.use_v10_fine_terrain = true;
    auto const fine_atlas = prepareAtlas(fine_config);
    ASSERT_NE(fine_atlas, nullptr);
    ASSERT_TRUE(fine_atlas->v10_provider->fine_terrain_cache);
    EXPECT_FALSE(fine_atlas->v10_provider->fine_terrain_cache->terrain);
    EXPECT_TRUE(expect_matches(*fine_atlas, 34'784.0, 30'212.0, 3U).river);
    EXPECT_TRUE(expect_matches(*fine_atlas, 34'784.0, 30'216.0, 3U).river);
    EXPECT_TRUE(expect_matches(*fine_atlas, 43'520.0, 9'216.0, 3U).lake);
    EXPECT_FALSE(fine_atlas->v10_provider->fine_terrain_cache->terrain);

    EXPECT_THROW((void)sampleV10WaterGeometry(GlobalAtlas{}, 0.0, 0.0, 0U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10WaterGeometry(*atlas, 0.0, 0.0, 4U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10WaterGeometry(*atlas,
        std::numeric_limits<double>::infinity(), 0.0, 0U), std::invalid_argument);
}

TEST(V10ProviderTests, TypedSnapshotRejectsMixedModelsAndMalformedSource)
{
    GenerationSnapshot snapshot;
    snapshot.stage = GenerationStage::V10Platec;
    snapshot.resolution = 256U;
    auto& fields = snapshot.v10.emplace();
    fields.height_raw.assign(65'536U, 1.0F);
    fields.plate_ids.assign(65'536U, 0U);
    fields.plate_directions.push_back({0U, 1.0F, 0.0F});
    ASSERT_TRUE(isValidGenerationSnapshot(snapshot));
    snapshot.v21.emplace();
    EXPECT_FALSE(isValidGenerationSnapshot(snapshot));
    snapshot.v21.reset();
    fields.height_raw[0U] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(isValidGenerationSnapshot(snapshot));
    fields.height_raw[0U] = 1.0F;
    fields.plate_ids.pop_back();
    EXPECT_FALSE(isValidGenerationSnapshot(snapshot));
}

TEST(V10ProviderTests, ActualProviderUsesCanonicalSameCoordinateHeightAtEveryLod)
{
    size_t snapshots = 0U;
    bool invalid_snapshot = false;
    bool complete = false;
    std::vector<GenerationProgress> climate_progress;
    std::vector<GenerationProgress> water_progress;
    auto atlas = prepareAtlas(configuration(), {}, [&](GenerationProgress const& progress) {
        complete |= progress.stage == GenerationStage::Complete;
        if (progress.stage == GenerationStage::V10Climate) {
            climate_progress.push_back(progress);
        }
        if (progress.stage == GenerationStage::V10Water) {
            water_progress.push_back(progress);
        }
        if (progress.snapshot)
        {
            ++snapshots;
            invalid_snapshot |= !isValidGenerationSnapshot(*progress.snapshot);
        }
    });
    ASSERT_TRUE(atlas);
    ASSERT_TRUE(atlas->v10_provider);
    auto const& provider = *atlas->v10_provider;
    ASSERT_TRUE(provider.fine_terrain_cache);
    EXPECT_FALSE(provider.fine_terrain_cache->terrain);
    EXPECT_EQ(provider.fine_terrain_cache->prepared_parent_queries, 0U);
    auto const default_surface = sampleV10SurfacePhases(*atlas, 46'336.0, 9'984.0, 3U);
    EXPECT_TRUE(std::isfinite(default_surface.front().elevation_m));
    EXPECT_FALSE(provider.fine_terrain_cache->terrain);
    EXPECT_EQ(provider.fine_terrain_cache->prepared_parent_queries, 0U);

    static constexpr size_t FINE_QUERY_COUNT = 8U;
    std::barrier fine_query_gate{static_cast<ptrdiff_t>(FINE_QUERY_COUNT)};
    std::array<double, FINE_QUERY_COUNT> fine_heights{};
    std::array<std::exception_ptr, FINE_QUERY_COUNT> fine_query_errors{};
    std::array<std::thread, FINE_QUERY_COUNT> fine_query_workers;
    for (size_t index = 0U; index < FINE_QUERY_COUNT; ++index)
    {
        fine_query_workers[index] = std::thread([&, index] {
            fine_query_gate.arrive_and_wait();
            try
            {
                fine_heights[index] = sampleV10FineTerrainSurface(
                    *atlas, 46'336.0, 9'984.0, 3U).elevation_m;
            }
            catch (...)
            {
                fine_query_errors[index] = std::current_exception();
            }
        });
    }
    for (std::thread& worker : fine_query_workers)
    {
        worker.join();
    }
    for (std::exception_ptr const& error : fine_query_errors)
    {
        EXPECT_FALSE(error);
    }
    for (double const height : fine_heights)
    {
        EXPECT_DOUBLE_EQ(height, fine_heights.front());
    }

    auto const fine_metadata = v10FineTerrainMetadata(*atlas);
    EXPECT_TRUE(provider.fine_terrain_cache->terrain);
    EXPECT_EQ(provider.fine_terrain_cache->prepared_parent_queries, fine_metadata.prepared_parent_queries);
    EXPECT_EQ(fine_metadata.prepared_parent_queries, 591'296U);
    EXPECT_NE(fine_metadata.source_id.find("6365ed7e79067c12fbc4d717e17ce2e1a239fbb672abf170ecf9cf4b1c255717"),
        std::string::npos);
    EXPECT_EQ(fine_metadata.parent_identity_sha256.size(), 64U);
    EXPECT_EQ(fine_metadata.field_identity_sha256.size(), 64U);
    EXPECT_EQ(fine_metadata.l2_identity_sha256.size(), 64U);
    EXPECT_EQ(fine_metadata.l3_identity_sha256.size(), 64U);
    EXPECT_EQ(fine_metadata.plan_sha256.size(), 64U);
    EXPECT_NE(fine_metadata.l2_identity_sha256, fine_metadata.l3_identity_sha256);
    EXPECT_EQ(fine_metadata.mountain_site_count, 64U);
    EXPECT_GT(fine_metadata.valley_network_count, 0U);
    RecordProperty("fine_source_id", fine_metadata.source_id);
    RecordProperty("fine_parent_identity_sha256", fine_metadata.parent_identity_sha256);
    RecordProperty("fine_field_identity_sha256", fine_metadata.field_identity_sha256);
    RecordProperty("fine_l2_identity_sha256", fine_metadata.l2_identity_sha256);
    RecordProperty("fine_l3_identity_sha256", fine_metadata.l3_identity_sha256);
    RecordProperty("fine_plan_sha256", fine_metadata.plan_sha256);
    RecordProperty("fine_prepared_parent_queries", fine_metadata.prepared_parent_queries);
    RecordProperty("fine_mountain_site_count", fine_metadata.mountain_site_count);
    RecordProperty("fine_valley_network_count", fine_metadata.valley_network_count);
    RecordProperty("fine_regional_site_count", fine_metadata.regional_site_count);
    RecordProperty("fine_regional_draws_sha256", fine_metadata.regional_draws_sha256);
    RecordProperty("fine_regional_feature_sha256", fine_metadata.regional_feature_sha256);
    EXPECT_TRUE(isValidAtlas(*atlas));
    EXPECT_TRUE(complete);
    EXPECT_GT(snapshots, 2U);
    EXPECT_FALSE(invalid_snapshot);
    ASSERT_FALSE(climate_progress.empty());
    uint32_t completed_climate = 0U;
    for (auto const& progress : climate_progress) {
        EXPECT_EQ(progress.total, 3'000U);
        EXPECT_GE(progress.completed, completed_climate);
        EXPECT_LE(progress.completed, progress.total);
        completed_climate = progress.completed;
    }
    ASSERT_TRUE(climate_progress.back().snapshot);
    EXPECT_EQ(climate_progress.back().completed, 3'000U);
    ASSERT_FALSE(water_progress.empty());
    uint32_t completed_water = 0U;
    for (auto const& progress : water_progress) {
        EXPECT_EQ(progress.total, 8U);
        EXPECT_GE(progress.completed, completed_water);
        EXPECT_LE(progress.completed, progress.total);
        completed_water = progress.completed;
    }
    ASSERT_TRUE(water_progress.back().snapshot);
    EXPECT_EQ(water_progress.back().completed, 8U);
    EXPECT_EQ(v10CurrentPlateIds(*atlas).size(), 65'536U);
    EXPECT_FALSE(v10PlateDirections(*atlas).empty());
    EXPECT_FALSE(v10RegisteredFields(*atlas).empty());
    // Capture registered source fields and the continuous evaluator basis.
    auto capture = [&] {
        std::vector<std::vector<std::byte>> copies;
        for (auto const& field : v10RegisteredFields(*atlas))
        {
            std::visit([&](auto values) {
                auto const bytes = std::as_bytes(values);
                copies.emplace_back(bytes.begin(), bytes.end());
            }, field.values);
        }
        auto const& basis = atlas->v10_provider->foundation;
        for (auto const* values : {&basis.land_magnitude_m, &basis.ocean_magnitude_m,
                 &basis.coast_distance_m, &basis.regional_relief_m})
        {
            auto const bytes = std::as_bytes(std::span{*values});
            copies.emplace_back(bytes.begin(), bytes.end());
        }
        return copies;
    };
    auto const before = capture();
    std::vector<std::array<double, 2U>> points = {
        {46'336.0, 9984.0}, {46'336.0 + 65'536.0, 9984.0 - 65'536.0},
        {0.0, 0.0}, {-1e-300, -1e-300}, {65'536.0, -65'536.0},
    };
    // Locate a real owner-zero sample, including owner zero in the public contract.
    bool found_zero_owner = false;
    for (uint32_t y = 0U; y < 256U && !found_zero_owner; ++y)
    {
        for (uint32_t x = 0U; x < 256U; ++x)
        {
            double const px = x * 256.0;
            double const py = y * 256.0;
            if (atlas->v10_provider->water.refinement.standing(px, py).second == 0)
            {
                points.push_back({px, py});
                found_zero_owner = true;
                break;
            }
        }
    }
    ASSERT_TRUE(found_zero_owner);
    auto const native = queryV10NativeBedSamples(*atlas, points);
    ASSERT_EQ(native.size(), points.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
        EXPECT_EQ(native[i].xy_m, points[i]);
        double const x = (i == 3U || i == 4U) ? 0.0 : std::fmod(points[i][0U], 65'536.0);
        double const y = (i == 3U || i == 4U) ? 0.0 : std::fmod(points[i][1U] + 65'536.0, 65'536.0);
        auto const standing = atlas->v10_provider->water.refinement.standing(x, y);
        EXPECT_DOUBLE_EQ(native[i].foundation_m, detail::sampleV10FoundationHeight(atlas->v10_provider->foundation, x, y));
        EXPECT_DOUBLE_EQ(native[i].standing_stage_m, standing.first);
        EXPECT_EQ(native[i].standing_owner, standing.second);
        EXPECT_GE(native[i].standing_owner, 0);
        EXPECT_TRUE(std::isfinite(native[i].foundation_m));
    }
    EXPECT_EQ(native.back().standing_owner, 0);
    EXPECT_DOUBLE_EQ(native[0].foundation_m, native[1].foundation_m);
    EXPECT_EQ(native[0].standing_owner, native[1].standing_owner);
    EXPECT_DOUBLE_EQ(native[2].foundation_m, native[3].foundation_m);
    EXPECT_EQ(native[2].standing_owner, native[3].standing_owner);
    EXPECT_TRUE(queryV10NativeBedSamples(*atlas, {}).empty());
    EXPECT_THROW((void)queryV10NativeBedSamples(GlobalAtlas{}, {}), std::invalid_argument);
    std::array<std::array<double, 2U>, 1U> invalid = {{{std::numeric_limits<double>::infinity(), 0.0}}};
    EXPECT_THROW((void)queryV10NativeBedSamples(*atlas, invalid), std::invalid_argument);
    invalid[0] = {0.0, std::numeric_limits<double>::quiet_NaN()};
    EXPECT_THROW((void)queryV10NativeBedSamples(*atlas, invalid), std::invalid_argument);
    std::stop_source native_stop;
    native_stop.request_stop();
    EXPECT_THROW((void)queryV10NativeBedSamples(*atlas, points, native_stop.get_token()), GenerationCancelled);
    EXPECT_THROW((void)queryV10NativeBedSamples(*atlas, {}, native_stop.get_token()), GenerationCancelled);
    EXPECT_EQ(capture(), before);
    auto const field = v10RiverBedField(*atlas);
    ASSERT_TRUE(field);
    EXPECT_EQ(field, v10RiverBedField(*atlas));
    EXPECT_THROW((void)v10RiverBedField(GlobalAtlas{}), std::invalid_argument);
    // Qualified generic reference: role-maximum-discharge-confluence-1m/after.npz.
    double const confluence = v10L0ElevationAt(*atlas, 34'816.0, 30'208.0);
    EXPECT_NEAR(confluence, 4.379'650'743'625'715, 1e-9);
    std::array<std::array<double, 2U>, 1U> const confluence_xy = {{{34'816.0, 30'208.0}}};
    EXPECT_NEAR(queryV10NativeBedSamples(*atlas, confluence_xy).front().foundation_m, 6.777'193'053'833'935, 1e-9);
    for (uint32_t lod = 0U; lod < 4U; ++lod)
    {
        EXPECT_DOUBLE_EQ(sampleV10Surface(*atlas, 34'816.0, 30'208.0, lod).elevation_m, confluence);
    }
    // Retained ordinary confluence: the curved bed is several metres north of
    // the inherited half-metre straight tube. River presentation follows it.
    auto const river_field = v10RiverBedField(*atlas);
    auto const& prepared_water = atlas->v10_provider->water;
    std::array<std::array<double, 2U>, 2U> const river_xy = {{{34'784.0, 30'212.0}, {34'784.0, 30'216.0}}};
    std::array<double, 2U> const expected_bed{10.108'730'892'793'931, 11.484'162'894'836'853};
    // The dominant wet reach must win independently of candidate order.
    auto const reaches = prepared_water.refinement.reaches();
    std::vector<size_t> wet_reaches;
    for (size_t const index : prepared_water.fixed_support.river_reach_order) {
        if (reaches[index].water_stage0 > expected_bed.front() + 1.0) {
            wet_reaches.push_back(index);
        }
    }
    ASSERT_GE(wet_reaches.size(), 2U);
    size_t const dominant_index = wet_reaches.front();
    size_t const other_index = wet_reaches.back();
    ASSERT_GT(reaches[dominant_index].discharge_m3_s, reaches[other_index].discharge_m3_s);
    auto const candidate = [&](size_t const index) {
        return river_field_candidate::NearestCourseGeometry{
            .within_nominal_channel = true,
            .source_reach_index = index,
            .source_start_vertex = reaches[index].start,
            .source_target_vertex = reaches[index].target,
        };
    };
    std::array overlap{candidate(other_index), candidate(dominant_index)};
    for (size_t order = 0U; order < overlap.size(); ++order) {
        auto const wet = detail::queryV10WaterPoint(prepared_water,
            river_xy.front()[0U], river_xy.front()[1U], expected_bed.front(), overlap);
        ASSERT_TRUE(wet);
        EXPECT_TRUE(wet->river);
        EXPECT_EQ(wet->river_owner_id, reaches[dominant_index].start);
        EXPECT_FLOAT_EQ(wet->water_level_m, static_cast<float>(reaches[dominant_index].water_stage0));
        std::reverse(overlap.begin(), overlap.end());
    }
    auto const river_native = queryV10NativeBedSamples(*atlas, river_xy);
    for (size_t i = 0U; i < river_xy.size(); ++i) {
        auto const& point = river_native[i];
        river_field_candidate::NativePointSample const input{{point.xy_m[0], point.xy_m[1]},
            point.foundation_m, point.standing_stage_m, point.standing_owner};
        auto const channels = river_field_candidate::sampleRiverChannel(*river_field, std::span{&input, 1U});
        ASSERT_TRUE(channels);
        auto const& channel = channels->front();
        EXPECT_NEAR(channel.bed.bed_m, expected_bed[i], 1e-9);
        ASSERT_FALSE(channel.containing_courses.empty());
        auto const legacy = detail::queryV10WaterPoint(prepared_water, point.xy_m[0], point.xy_m[1], channel.bed.bed_m);
        ASSERT_TRUE(legacy);
        EXPECT_FALSE(legacy->river);
        auto const corrected = detail::queryV10WaterPoint(prepared_water,
            point.xy_m[0], point.xy_m[1], channel.bed.bed_m, channel.containing_courses);
        ASSERT_TRUE(corrected);
        ASSERT_TRUE(corrected->river);
        EXPECT_GT(corrected->water_level_m, channel.bed.bed_m);
        bool stage_checked = false;
        for (auto const& course : channel.containing_courses) {
            auto const& reach = prepared_water.refinement.reaches()[course.source_reach_index];
            if (reach.start != corrected->river_owner_id || course.receiver_extension)
            {
                continue;
            }
            EXPECT_EQ(course.source_start_vertex, reach.start);
            EXPECT_EQ(course.source_target_vertex, reach.target);
            EXPECT_EQ(corrected->water_level_m, static_cast<float>(reach.water_stage0
                + course.course_fraction * (reach.water_stage1 - reach.water_stage0)));
            double const stage = reach.water_stage0
                + course.course_fraction * (reach.water_stage1 - reach.water_stage0);
            auto const shallow = detail::queryV10WaterPoint(prepared_water, point.xy_m[0], point.xy_m[1],
                stage - 1e-8, std::span{&course, 1U});
            ASSERT_TRUE(shallow);
            EXPECT_FALSE(shallow->river);
            auto invalid_course = course;
            invalid_course.source_reach_index = prepared_water.refinement.reaches().size();
            auto const invalid = detail::queryV10WaterPoint(prepared_water, point.xy_m[0], point.xy_m[1],
                channel.bed.bed_m, std::span{&invalid_course, 1U});
            ASSERT_FALSE(invalid);
            EXPECT_EQ(invalid.error().kind, detail::V9WaterFailure::Kind::InvalidInput);
            stage_checked = true;
        }
        EXPECT_TRUE(stage_checked);
        for (uint32_t lod = 0U; lod < 4U; ++lod) {
            auto const sample = sampleV10Surface(*atlas, point.xy_m[0], point.xy_m[1], lod);
            EXPECT_TRUE(sample.river);
            EXPECT_DOUBLE_EQ(sample.elevation_m, channel.bed.bed_m);
            EXPECT_EQ(sample.water_level_m, corrected->water_level_m);
        }
        for (uint32_t lod : {2U, 3U}) {
            auto const fine = sampleV10FineTerrainSurface(*atlas, point.xy_m[0], point.xy_m[1], lod);
            EXPECT_TRUE(fine.river);
            EXPECT_DOUBLE_EQ(fine.elevation_m, channel.bed.bed_m);
            EXPECT_EQ(fine.water_level_m, corrected->water_level_m);
            auto const fine_water = detail::queryV10WaterPoint(prepared_water,
                point.xy_m[0], point.xy_m[1], fine.elevation_m, channel.containing_courses);
            ASSERT_TRUE(fine_water);
            EXPECT_EQ(fine_water->river_owner_id, corrected->river_owner_id);
            EXPECT_EQ(fine_water->water_level_m, corrected->water_level_m);
            EXPECT_EQ(fine_water->water_depth_m, corrected->water_depth_m);
        }
        auto const map = queryV10SurfaceGrid(*atlas, {1U, 1U, point.xy_m[0], point.xy_m[1], 1.0, 0U, 0U});
        ASSERT_EQ(map.size(), 1U);
        EXPECT_TRUE(map.front().river);
        EXPECT_DOUBLE_EQ(map.front().elevation_m, channel.bed.bed_m);
        EXPECT_EQ(mapV10ElevationToBlock(map.front().elevation_m), mapV10ElevationToBlock(expected_bed[i]));
        auto const wrapped = sampleV10Surface(*atlas, point.xy_m[0] - 65'536.0, point.xy_m[1] + 65'536.0, 3U);
        EXPECT_TRUE(wrapped.river);
        EXPECT_DOUBLE_EQ(wrapped.elevation_m, channel.bed.bed_m);
        EXPECT_EQ(wrapped.water_level_m, corrected->water_level_m);
    }
    auto const lake = sampleV10Surface(*atlas, 43'520.0, 9216.0, 3U);
    EXPECT_TRUE(lake.lake);
    EXPECT_FALSE(lake.river);
    auto const lake_native = detail::queryV10WaterPoint(prepared_water, 43'520.0, 9216.0, lake.elevation_m);
    ASSERT_TRUE(lake_native);
    EXPECT_EQ(lake.water_level_m, lake_native->water_level_m);
    for (uint32_t lod : {2U, 3U}) {
        auto const fine_lake = sampleV10FineTerrainSurface(*atlas, 43'520.0, 9216.0, lod);
        EXPECT_DOUBLE_EQ(fine_lake.elevation_m, lake.elevation_m);
        EXPECT_EQ(fine_lake.water_level_m, lake.water_level_m);
        EXPECT_EQ(fine_lake.lake, lake.lake);
        EXPECT_EQ(fine_lake.river, lake.river);
        auto const fine_lake_support = detail::queryV10WaterPoint(prepared_water,
            43'520.0, 9216.0, fine_lake.elevation_m);
        ASSERT_TRUE(fine_lake_support);
        EXPECT_EQ(fine_lake_support->lake_id, lake_native->lake_id);
        EXPECT_EQ(fine_lake_support->water_level_m, lake_native->water_level_m);
    }
    auto const receiver = sampleV10Surface(*atlas, 34'860.0, 30'208.0, 3U);
    EXPECT_TRUE(receiver.ocean);
    EXPECT_FALSE(receiver.river);
    std::array<std::array<double, 2U>, 1U> const summit_xy = {{{46'336.0, 9984.0}}};
    auto const summit_native = queryV10NativeBedSamples(*atlas, summit_xy);
    EXPECT_DOUBLE_EQ(v10L0ElevationAt(*atlas, 46'336.0, 9984.0), summit_native.front().foundation_m);
    double const summit = v10L0ElevationAt(*atlas, 46'336.0, 9984.0);
    EXPECT_NEAR(summit, 723.342'269'667'154'6, 1e-9);
    EXPECT_NEAR(v10L0ElevationAt(*atlas, 46'357.0, 10'018.0), 726.704'491'978'525'8, 1e-9);
    constexpr double FINE_RIDGE_X = 52'372.486'699'750'08;
    constexpr double FINE_RIDGE_Y = 4'937.492'867'001'128;
    double const fine_ridge_reference = v10L0ElevationAt(*atlas, FINE_RIDGE_X, FINE_RIDGE_Y);
    auto const fine_ridge_map = queryV10SurfaceGrid(*atlas,
        {1U, 1U, FINE_RIDGE_X, FINE_RIDGE_Y, 1.0, 3U, 0U});
    ASSERT_EQ(fine_ridge_map.size(), 1U);
    auto const fine_ridge_l2 = sampleV10FineTerrainSurface(*atlas, FINE_RIDGE_X, FINE_RIDGE_Y, 2U);
    auto const fine_ridge_l3 = sampleV10FineTerrainSurface(*atlas, FINE_RIDGE_X, FINE_RIDGE_Y, 3U);
    auto const fine_ridge_surface = sampleV10Surface(*atlas, FINE_RIDGE_X, FINE_RIDGE_Y, 3U);
    RecordProperty("fine_ridge_x_m", FINE_RIDGE_X);
    RecordProperty("fine_ridge_y_m", FINE_RIDGE_Y);
    RecordProperty("fine_ridge_reference_m", fine_ridge_reference);
    RecordProperty("fine_ridge_map_reference_m", fine_ridge_map.front().elevation_m);
    RecordProperty("fine_ridge_l2_m", fine_ridge_l2.elevation_m);
    RecordProperty("fine_ridge_l3_m", fine_ridge_l3.elevation_m);
    EXPECT_DOUBLE_EQ(fine_ridge_map.front().elevation_m, fine_ridge_reference);
    EXPECT_DOUBLE_EQ(fine_ridge_surface.elevation_m, fine_ridge_reference);
    EXPECT_FALSE(fine_ridge_l2.ocean || fine_ridge_l2.lake || fine_ridge_l2.river);
    EXPECT_FALSE(fine_ridge_l3.ocean || fine_ridge_l3.lake || fine_ridge_l3.river);
    EXPECT_GT(std::abs(fine_ridge_l2.elevation_m - fine_ridge_reference), 0.1);
    EXPECT_NE(fine_ridge_l2.elevation_m, fine_ridge_l3.elevation_m);
    for (uint32_t lod = 0U; lod < 4U; ++lod)
    {
        auto const sample = sampleV10Surface(*atlas, 46'336.0, 9984.0, lod);
        EXPECT_DOUBLE_EQ(sample.elevation_m, summit);
        EXPECT_TRUE(std::isfinite(sample.temperature_celsius));
        EXPECT_TRUE(std::isfinite(sample.precipitation_mm_year));
    }
    auto const phases = sampleV10SurfacePhases(*atlas, 46'336.0, 9984.0, 0U);
    for (uint32_t phase = 0U; phase < phases.size(); ++phase)
    {
        auto const selected = sampleV10Surface(*atlas, 46'336.0, 9984.0, 0U, phase);
        EXPECT_DOUBLE_EQ(selected.elevation_m, phases[phase].elevation_m);
        EXPECT_DOUBLE_EQ(selected.water_level_m, phases[phase].water_level_m);
        EXPECT_DOUBLE_EQ(selected.temperature_celsius, phases[phase].temperature_celsius);
        EXPECT_DOUBLE_EQ(selected.precipitation_mm_year, phases[phase].precipitation_mm_year);
        EXPECT_DOUBLE_EQ(selected.snow_water_equivalent_m, phases[phase].snow_water_equivalent_m);
        EXPECT_DOUBLE_EQ(selected.glacier_thickness_m, phases[phase].glacier_thickness_m);
        EXPECT_DOUBLE_EQ(selected.sea_ice_thickness_m, phases[phase].sea_ice_thickness_m);
        EXPECT_EQ(selected.biome_id, phases[phase].biome_id);
        EXPECT_EQ(selected.ocean, phases[phase].ocean);
        EXPECT_EQ(selected.lake, phases[phase].lake);
        EXPECT_EQ(selected.river, phases[phase].river);
    }
    auto const annual = sampleV10AnnualMeanSurface(*atlas, 46'336.0, 9984.0, 0U);
    auto mean = [&phases](double V10SurfaceSample::* member) {
        double sum = 0.0;
        for (auto const& phase : phases)
        {
            sum += phase.*member;
        }
        return sum / static_cast<double>(phases.size());
    };
    EXPECT_DOUBLE_EQ(annual.temperature_celsius, mean(&V10SurfaceSample::temperature_celsius));
    EXPECT_DOUBLE_EQ(annual.precipitation_mm_year, mean(&V10SurfaceSample::precipitation_mm_year));
    EXPECT_DOUBLE_EQ(annual.snow_water_equivalent_m, mean(&V10SurfaceSample::snow_water_equivalent_m));
    EXPECT_DOUBLE_EQ(annual.glacier_thickness_m, mean(&V10SurfaceSample::glacier_thickness_m));
    EXPECT_DOUBLE_EQ(annual.sea_ice_thickness_m, mean(&V10SurfaceSample::sea_ice_thickness_m));
    EXPECT_DOUBLE_EQ(annual.elevation_m, phases.front().elevation_m);
    EXPECT_DOUBLE_EQ(annual.water_level_m, phases.front().water_level_m);
    EXPECT_EQ(annual.biome_id, phases.front().biome_id);
    EXPECT_EQ(annual.ocean, phases.front().ocean);
    EXPECT_EQ(annual.lake, phases.front().lake);
    EXPECT_EQ(annual.river, phases.front().river);
    // An ocean inventory witness prevents a dry summit's zero ice from hiding
    // a regression that always selects the first sea-ice phase.
    bool checked_ocean_ice = false;
    for (auto const& field : v10RegisteredFields(*atlas))
    {
        if (field.name != "sea_ice_thickness_m")
        {
            continue;
        }
        auto const ice = std::get<std::span<float const>>(field.values);
        auto const peak = std::max_element(ice.begin(), ice.end());
        ASSERT_NE(peak, ice.end());
        size_t const vertex = static_cast<size_t>(peak - ice.begin()) % (256U * 256U);
        double const x = static_cast<double>(vertex % 256U) * 256.0;
        double const y = static_cast<double>(vertex / 256U) * 256.0;
        auto const ocean_phases = sampleV10SurfacePhases(*atlas, x, y, 0U);
        auto const ocean_annual = sampleV10AnnualMeanSurface(*atlas, x, y, 0U);
        ASSERT_TRUE(ocean_annual.ocean);
        double ice_sum = 0.0;
        for (auto const& phase : ocean_phases)
        {
            ice_sum += phase.sea_ice_thickness_m;
        }
        EXPECT_GT(ice_sum, 0.0);
        EXPECT_DOUBLE_EQ(ocean_annual.sea_ice_thickness_m, ice_sum / 8.0);
        checked_ocean_ice = true;
    }
    EXPECT_TRUE(checked_ocean_ice);
    EXPECT_THROW((void)sampleV10AnnualMeanSurface(*atlas, 0.0, 0.0, 4U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10SurfacePhases(*atlas, 0.0, 0.0, 4U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10SurfacePhases(*atlas,
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0U), std::invalid_argument);
    EXPECT_DOUBLE_EQ(v10L0ElevationAt(*atlas, 46'336.0 + 65'536.0, 9984.0 - 65'536.0), summit);
    auto const grid = queryV10SurfaceGrid(*atlas, {1U, 1U, 46'336.0, 9984.0, 1.0, 3U, 0U});
    ASSERT_EQ(grid.size(), 1U);
    EXPECT_DOUBLE_EQ(grid.front().elevation_m, summit);
    EXPECT_THROW((void)sampleV10Surface(*atlas, 0.0, 0.0, 4U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10Surface(*atlas, 0.0, 0.0, 0U, 8U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10FineTerrainSurface(*atlas, 0.0, 0.0, 0U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10FineTerrainSurface(*atlas, 0.0, 0.0, 4U), std::invalid_argument);
    EXPECT_THROW((void)sampleV10FineTerrainSurface(*atlas, 0.0, 0.0, 2U, 8U), std::invalid_argument);
    EXPECT_THROW((void)queryV10SurfaceGrid(*atlas, {}), std::invalid_argument);
    EXPECT_THROW((void)v10L0ElevationAt(*atlas, std::numeric_limits<double>::infinity(), 0.0), std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW((void)queryV10SurfaceGrid(*atlas, {1U, 1U, 0.0, 0.0, 1.0}, stop.get_token()), GenerationCancelled);
    size_t const diagnostic_grids = exportFineTerrainDiagnosticGrids(*atlas);
    RecordProperty("fine_diagnostic_grid_count", diagnostic_grids);
    char const* diagnostic_directory = std::getenv("MC_V10_FINE_DIAGNOSTIC_DIR");
    if (diagnostic_directory != nullptr && *diagnostic_directory != '\0') {
        EXPECT_EQ(diagnostic_grids, 10U);
    }
    atlas.reset();
    auto const after_atlas = river_field_candidate::sampleRiverBed(*field, {});
    ASSERT_TRUE(after_atlas);
    EXPECT_TRUE(after_atlas->empty());
}
TEST(V10ProviderTests, RiverConstraintExportRetainsPeriodicSeamCourses)
{
    namespace river = river_field_candidate;
    river::RiverFieldIdentity const identity{"test", "test", "test", "test", "test", "test", "test", "test", 42U, 65'536.0};
    std::array<river::RiverReach, 1U> const reaches = {{{1, 2, {65'520.0, 100.0}, {32.0, 0.0},
        50.0, 49.0, 1.0, false}}};
    size_t sampler_calls = 0U;
    river::NativeBatchSampler const sampler = [&](std::span<river::WorldXY const> xy)
        -> std::expected<std::vector<river::NativePointSample>, river::Failure> {
        ++sampler_calls;
        std::vector<river::NativePointSample> result;
        for (auto point : xy)
        {
            result.push_back({point, 100.0, 0.0, 0});
        }
        return result;
    };
    auto field = river::prepareRiverField(identity, reaches, {}, sampler, {}, {});
    ASSERT_TRUE(field) << (field ? "" : field.error().message);
    auto original = river::exportRiverConstraints(**field,
        {{65'500.0, 60.0}, {65'560.0, 140.0}, river::ConsumerKind::L1, 8.0}, sampler);
    auto shifted = river::exportRiverConstraints(**field,
        {{-36.0, 60.0}, {24.0, 140.0}, river::ConsumerKind::L1, 8.0}, sampler);
    ASSERT_TRUE(original);
    ASSERT_TRUE(shifted);
    ASSERT_FALSE(original->polylines.empty());
    ASSERT_EQ(original->polylines.size(), shifted->polylines.size());
    for (size_t p = 0; p < original->polylines.size(); ++p)
    {
        auto const& a = original->polylines[p];
        auto const& b = shifted->polylines[p];
        EXPECT_EQ(a.role, b.role);
        EXPECT_EQ(a.course_index, b.course_index);
        ASSERT_EQ(a.vertices.size(), b.vertices.size());
        for (size_t i = 0; i < a.vertices.size(); ++i)
        {
            EXPECT_NEAR(a.vertices[i].native.xy.x_m - 65'536.0, b.vertices[i].native.xy.x_m, 1e-9);
            EXPECT_DOUBLE_EQ(a.vertices[i].native.xy.y_m, b.vertices[i].native.xy.y_m);
            EXPECT_NEAR(a.vertices[i].bed_m, b.vertices[i].bed_m, 1e-9);
        }
    }
    auto full_period = river::exportRiverConstraints(**field,
        {{0.0, 60.0}, {65'536.0, 140.0}, river::ConsumerKind::L1, 8.0}, sampler);
    ASSERT_TRUE(full_period);
    EXPECT_FALSE(full_period->polylines.empty());
    sampler_calls = 0U;
    auto empty = river::exportRiverConstraints(**field,
        {{1000.0, 1000.0}, {1064.0, 1064.0}, river::ConsumerKind::L1, 8.0}, sampler);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->polylines.empty());
    EXPECT_EQ(sampler_calls, 0U);
}

TEST(V10ProviderTests, CurvedChannelCandidatesRetainSourceOrderAndOverlappingMembership)
{
    namespace river = river_field_candidate;
    river::RiverFieldIdentity const identity{"test", "test", "test", "test", "test", "test", "test", "test", 42U, 65'536.0};
    // Deliberately reverse source order. The nearest narrow course excludes the
    // point, while the wider, more distant course contains it.
    std::array<river::RiverReach, 2U> const reaches = {{{20, 21, {100.0, 103.0}, {256.0, 0.0},
        50.0, 49.0, 0.01, false}, {10, 11, {100.0, 100.0}, {256.0, 0.0}, 50.0, 49.0, 1.0, false}}};
    river::NativeBatchSampler const sampler = [](std::span<river::WorldXY const> xy)
        -> std::expected<std::vector<river::NativePointSample>, river::Failure> {
        std::vector<river::NativePointSample> result;
        for (auto point : xy)
        {
            result.push_back({point, 100.0, 0.0, 0});
        }
        return result;
    };
    auto const field = river::prepareRiverField(identity, reaches, {}, sampler, {}, {});
    ASSERT_TRUE(field);
    std::array<river::NativePointSample, 2U> const points = {{{{228.0, 107.0}, 100.0, 0.0, 0},
        {{228.0 + 65'536.0, 107.0 - 65'536.0}, 100.0, 0.0, 0}}};
    auto const channels = river::sampleRiverChannel(**field, points);
    auto const beds = river::sampleRiverBed(**field, points);
    ASSERT_TRUE(channels);
    ASSERT_TRUE(beds);
    for (size_t i = 0U; i < points.size(); ++i) {
        auto const& sample = (*channels)[i];
        ASSERT_TRUE(sample.nearest_course);
        EXPECT_FALSE(sample.nearest_course->within_nominal_channel);
        EXPECT_EQ(sample.nearest_course->source_reach_index, 0U);
        ASSERT_EQ(sample.containing_courses.size(), 1U);
        auto const& containing = sample.containing_courses.front();
        EXPECT_EQ(containing.source_reach_index, 1U);
        EXPECT_EQ(containing.source_start_vertex, 10);
        EXPECT_EQ(containing.source_target_vertex, 11);
        EXPECT_DOUBLE_EQ(containing.course_fraction, 0.5);
        EXPECT_FALSE(containing.receiver_extension);
        EXPECT_DOUBLE_EQ(containing.centreline_distance_m, 7.0);
        EXPECT_DOUBLE_EQ(sample.bed.bed_m, (*beds)[i].bed_m);
        EXPECT_DOUBLE_EQ(sample.bed.signed_delta_m, (*beds)[i].signed_delta_m);
        EXPECT_DOUBLE_EQ(sample.bed.corridor_support, (*beds)[i].corridor_support);
    }
    river::NativePointSample const valley{{228.0, 120.0}, 100.0, 0.0, 0};
    auto const valley_channel = river::sampleRiverChannel(**field, std::span{&valley, 1U});
    ASSERT_TRUE(valley_channel);
    EXPECT_GT(valley_channel->front().bed.corridor_support, 0.0);
    EXPECT_TRUE(valley_channel->front().containing_courses.empty());
}

TEST(V10ProviderTests, CurvedChannelReceiverTailRetainsStandingOwner)
{
    namespace river = river_field_candidate;
    river::RiverFieldIdentity const identity{"test", "test", "test", "test", "test", "test", "test", "test", 42U, 65'536.0};
    std::array<river::RiverReach, 1U> const reaches = {{{1, 2, {100.0, 100.0}, {32.0, 0.0},
        2.0, -1.0, 1.0, true}}};
    river::NativeBatchSampler const sampler = [](std::span<river::WorldXY const> xy)
        -> std::expected<std::vector<river::NativePointSample>, river::Failure> {
        std::vector<river::NativePointSample> result;
        for (auto point : xy)
        {
            result.push_back({point, 130.0 - point.x_m, 0.0, 7});
        }
        return result;
    };
    auto const field = river::prepareRiverField(identity, reaches, {}, sampler, {}, {});
    ASSERT_TRUE(field) << (field ? "" : field.error().message);
    river::NativePointSample const point{{148.0, 100.0}, -18.0, 0.0, 7};
    auto const channel = river::sampleRiverChannel(**field, std::span{&point, 1U});
    auto const bed = river::sampleRiverBed(**field, std::span{&point, 1U});
    ASSERT_TRUE(channel);
    ASSERT_TRUE(bed);
    ASSERT_TRUE(channel->front().nearest_course);
    auto const& tail = *channel->front().nearest_course;
    EXPECT_TRUE(tail.receiver_extension);
    EXPECT_EQ(tail.receiver_owner, 7);
    EXPECT_EQ(tail.source_reach_index, 0U);
    EXPECT_DOUBLE_EQ(tail.course_fraction, 0.5);
    EXPECT_EQ(channel->front().native_standing_owner, 7);
    EXPECT_DOUBLE_EQ(channel->front().bed.bed_m, bed->front().bed_m);
}

TEST(V10ProviderTests, FineSurfaceSkipsFineFieldForExactRiverAndStandingBed)
{
    GenerationConfiguration config = configuration();
    config.use_v10_fine_terrain = true;
    auto const atlas = prepareAtlas(config);
    ASSERT_TRUE(atlas);
    ASSERT_TRUE(atlas->v10_provider);
    ASSERT_TRUE(atlas->v10_provider->fine_terrain_cache);
    auto const& cache = *atlas->v10_provider->fine_terrain_cache;
    ASSERT_FALSE(cache.terrain);
    EXPECT_EQ(cache.prepared_parent_queries, 0U);

    auto const expect_exact_bed = [&](double const x_m, double const y_m,
        bool const expected_lake, bool const expected_river) {
        V10SurfaceSample const sample = sampleV10FineTerrainSurface(*atlas, x_m, y_m, 3U);
        EXPECT_FALSE(sample.ocean);
        EXPECT_EQ(sample.lake, expected_lake);
        EXPECT_EQ(sample.river, expected_river);
        EXPECT_DOUBLE_EQ(sample.elevation_m, v10L0ElevationAt(*atlas, x_m, y_m));
        EXPECT_FALSE(cache.terrain);
        EXPECT_EQ(cache.prepared_parent_queries, 0U);
    };
    expect_exact_bed(34'784.0, 30'212.0, false, true);
    expect_exact_bed(34'784.0, 30'216.0, false, true);
    expect_exact_bed(43'520.0, 9'216.0, true, false);

    V10SurfaceSample const dry_ridge = sampleV10FineTerrainSurface(
        *atlas, 52'372.486'699'750'08, 4'937.492'867'001'128, 3U);
    EXPECT_FALSE(dry_ridge.ocean || dry_ridge.lake || dry_ridge.river);
    EXPECT_TRUE(std::isfinite(dry_ridge.elevation_m));
    EXPECT_TRUE(cache.terrain);
    EXPECT_EQ(cache.prepared_parent_queries, 591'296U);
}

TEST(V10ProviderTests, ProfileFineWorldMapNativeCost)
{
    char const* const enabled = std::getenv("MC_V10_FINE_STARTUP_PROFILE");
    if (enabled == nullptr || std::string_view{enabled} != "1")
    {
        GTEST_SKIP() << "Set MC_V10_FINE_STARTUP_PROFILE=1 to run the bounded startup cost profile";
    }

    using Clock = std::chrono::steady_clock;
    struct PreparationProfile final
    {
        Clock::time_point started_at{};
        Clock::time_point finished_at{};
        std::optional<Clock::time_point> first_climate_progress;
        std::optional<Clock::time_point> last_climate_progress;
        std::optional<Clock::time_point> first_water_progress;
        std::optional<Clock::time_point> last_water_progress;
    };
    auto const prepare_profiled_atlas = [](GenerationConfiguration const& config,
        PreparationProfile& profile)
    {
        profile.started_at = Clock::now();
        auto const atlas = prepareAtlas(config, {}, [&](GenerationProgress const& progress) {
            Clock::time_point const observed_at = Clock::now();
            if (progress.stage == GenerationStage::V10Climate)
            {
                if (!profile.first_climate_progress)
                {
                    profile.first_climate_progress = observed_at;
                }
                profile.last_climate_progress = observed_at;
            }
            else if (progress.stage == GenerationStage::V10Water)
            {
                if (!profile.first_water_progress)
                {
                    profile.first_water_progress = observed_at;
                }
                profile.last_water_progress = observed_at;
            }
        });
        profile.finished_at = Clock::now();
        return atlas;
    };
    auto const elapsed_us = [](Clock::time_point const start, Clock::time_point const end) {
        return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    };

    PreparationProfile reference_profile;
    auto const reference_atlas = prepare_profiled_atlas(configuration(), reference_profile);
    ASSERT_TRUE(reference_atlas);

    GenerationConfiguration candidate_config = configuration();
    candidate_config.use_v10_fine_terrain = true;
    PreparationProfile candidate_profile;
    auto const candidate_atlas = prepare_profiled_atlas(candidate_config, candidate_profile);
    ASSERT_TRUE(candidate_atlas);
    ASSERT_TRUE(candidate_atlas->v10_provider);
    ASSERT_TRUE(candidate_atlas->v10_provider->fine_terrain_cache);
    EXPECT_FALSE(candidate_atlas->v10_provider->fine_terrain_cache->terrain);

    RecordProperty("reference_atlas_prepare_us",
        elapsed_us(reference_profile.started_at, reference_profile.finished_at));
    RecordProperty("candidate_atlas_prepare_us",
        elapsed_us(candidate_profile.started_at, candidate_profile.finished_at));
    ASSERT_TRUE(reference_profile.first_climate_progress);
    ASSERT_TRUE(reference_profile.last_climate_progress);
    ASSERT_TRUE(reference_profile.first_water_progress);
    ASSERT_TRUE(reference_profile.last_water_progress);
    ASSERT_TRUE(candidate_profile.first_climate_progress);
    ASSERT_TRUE(candidate_profile.last_climate_progress);
    ASSERT_TRUE(candidate_profile.first_water_progress);
    ASSERT_TRUE(candidate_profile.last_water_progress);
    RecordProperty("reference_climate_progress_us",
        elapsed_us(*reference_profile.first_climate_progress, *reference_profile.last_climate_progress));
    RecordProperty("reference_climate_to_first_water_us",
        elapsed_us(*reference_profile.last_climate_progress, *reference_profile.first_water_progress));
    RecordProperty("reference_water_progress_us",
        elapsed_us(*reference_profile.first_water_progress, *reference_profile.last_water_progress));
    RecordProperty("reference_water_last_progress_to_atlas_return_us",
        elapsed_us(*reference_profile.last_water_progress, reference_profile.finished_at));
    RecordProperty("candidate_climate_progress_us",
        elapsed_us(*candidate_profile.first_climate_progress, *candidate_profile.last_climate_progress));
    RecordProperty("candidate_climate_to_first_water_us",
        elapsed_us(*candidate_profile.last_climate_progress, *candidate_profile.first_water_progress));
    RecordProperty("candidate_water_progress_us",
        elapsed_us(*candidate_profile.first_water_progress, *candidate_profile.last_water_progress));
    RecordProperty("candidate_water_last_progress_to_atlas_return_us",
        elapsed_us(*candidate_profile.last_water_progress, candidate_profile.finished_at));

    // Match rasterizeS2NativeWorldMap's provider call count and L0 coordinates.
    static constexpr size_t SOURCE_RESOLUTION = 256U;
    constexpr size_t SOURCE_COUNT = SOURCE_RESOLUTION * SOURCE_RESOLUTION;
    auto const map_sample = [](GlobalAtlas const& atlas, size_t const index) {
        double const x_m = static_cast<double>(index % SOURCE_RESOLUTION) * 256.0;
        double const y_m = static_cast<double>(index / SOURCE_RESOLUTION) * 256.0;
        return sampleV10SurfacePhases(atlas, x_m, y_m, 0U);
    };
    double reference_checksum = 0.0;
    Clock::time_point const reference_map_started = Clock::now();
    for (size_t index = 0U; index < SOURCE_COUNT; ++index)
    {
        reference_checksum += map_sample(*reference_atlas, index).front().elevation_m;
    }
    Clock::time_point const reference_map_finished = Clock::now();

    double candidate_checksum = 0.0;
    Clock::time_point const candidate_first_sample_started = Clock::now();
    candidate_checksum += map_sample(*candidate_atlas, 0U).front().elevation_m;
    Clock::time_point const candidate_first_sample_finished = Clock::now();
    for (size_t index = 1U; index < SOURCE_COUNT; ++index)
    {
        candidate_checksum += map_sample(*candidate_atlas, index).front().elevation_m;
    }
    Clock::time_point const candidate_map_finished = Clock::now();

    V10FineTerrainMetadata const metadata = v10FineTerrainMetadata(*candidate_atlas);
    EXPECT_EQ(metadata.prepared_parent_queries, 591'296U);
    RecordProperty("map_source_sample_count", std::to_string(SOURCE_COUNT));
    RecordProperty("reference_map_samples_us", elapsed_us(reference_map_started, reference_map_finished));
    RecordProperty("candidate_first_sample_including_lazy_field_us",
        elapsed_us(candidate_first_sample_started, candidate_first_sample_finished));
    RecordProperty("candidate_remaining_map_samples_us",
        elapsed_us(candidate_first_sample_finished, candidate_map_finished));
    RecordProperty("candidate_map_total_us", elapsed_us(candidate_first_sample_started, candidate_map_finished));
    RecordProperty("candidate_fine_parent_query_count", std::to_string(metadata.prepared_parent_queries));
    RecordProperty("reference_map_height_checksum", std::to_string(reference_checksum));
    RecordProperty("candidate_map_height_checksum", std::to_string(candidate_checksum));
}

#if defined(MC_TEST_HAS_CPU_MAP)
TEST(V10ProviderTests, ProfileWorldPreparationReferenceVersusFine)
{
    char const* const enabled = std::getenv("MC_V10_WORLD_PREPARATION_PROFILE");
    if (enabled == nullptr || std::string_view{enabled} != "1")
    {
        GTEST_SKIP() << "Set MC_V10_WORLD_PREPARATION_PROFILE=1 to profile the V10 preparation service";
    }

    using Clock = std::chrono::steady_clock;
    auto const run_profile = [&](bool const use_fine, std::string_view const label) {
        GenerationConfiguration config = configuration();
        config.use_v10_fine_terrain = use_fine;
        Clock::time_point const started_at = Clock::now();
        server::WorldPreparation preparation{config, 66'666U, started_at};
        Clock::time_point next_status_poll = started_at;
        std::optional<Clock::time_point> last_progress_observed_at;
        Clock::duration max_progress_observation_gap{};
        Clock::duration max_ready_world_poll{};
        Clock::duration max_status_poll{};
        uint64_t ready_world_polls = 0U;
        uint64_t status_polls = 0U;
        uint64_t last_progress_sequence = 0U;
        std::shared_ptr<shared::TerrainWorld const> terminal_ready_world;
        server::WorldPreparationSnapshot last_snapshot;
        std::array<bool, 32U> recorded_stages{};

        auto const deadline = started_at + std::chrono::seconds{90};
        while (Clock::now() < deadline)
        {
            Clock::time_point const ready_poll_started = Clock::now();
            std::shared_ptr<shared::TerrainWorld const> const ready_world = preparation.readyWorld();
            Clock::duration const ready_poll_duration = Clock::now() - ready_poll_started;
            max_ready_world_poll = std::max(max_ready_world_poll, ready_poll_duration);
            ++ready_world_polls;
            if (ready_world)
            {
                terminal_ready_world = ready_world;
            }

            if (Clock::now() >= next_status_poll)
            {
                Clock::time_point const status_poll_started = Clock::now();
                last_snapshot = preparation.latest();
                Clock::duration const status_poll_duration = Clock::now() - status_poll_started;
                max_status_poll = std::max(max_status_poll, status_poll_duration);
                ++status_polls;
                next_status_poll = status_poll_started + std::chrono::milliseconds{100};

                if (last_snapshot.latest_progress
                    && last_snapshot.latest_progress->sequence != last_progress_sequence)
                {
                    auto const& latest_progress = *last_snapshot.latest_progress;
                    if (last_progress_observed_at)
                    {
                        max_progress_observation_gap = std::max(max_progress_observation_gap,
                            latest_progress.observed_at - *last_progress_observed_at);
                    }
                    last_progress_observed_at = latest_progress.observed_at;
                    last_progress_sequence = latest_progress.sequence;
                }

                for (server::WorldPreparationProgressMilestone const& milestone
                    : last_snapshot.progress_milestones)
                {
                    if (milestone.kind != server::WorldPreparationProgressMilestoneKind::StageBoundary)
                    {
                        continue;
                    }
                    size_t const stage_index = static_cast<size_t>(milestone.update.progress.stage);
                    if (stage_index >= recorded_stages.size() || recorded_stages[stage_index])
                    {
                        continue;
                    }
                    recorded_stages[stage_index] = true;
                    std::string const key = std::string{label} + "_stage_"
                        + std::to_string(stage_index) + "_at_us";
                    RecordProperty(key, std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                        milestone.update.observed_at - started_at).count()));
                }

                if (last_snapshot.state != server::WorldPreparationState::Preparing)
                {
                    if (last_snapshot.state == server::WorldPreparationState::Ready)
                    {
                        terminal_ready_world = last_snapshot.ready_world;
                    }
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }

        if (last_snapshot.state == server::WorldPreparationState::Preparing)
        {
            preparation.cancel();
            last_snapshot = preparation.latest();
        }
        std::string const prefix{label};
        RecordProperty(prefix + "_ready_world_poll_count", std::to_string(ready_world_polls));
        RecordProperty(prefix + "_status_poll_count", std::to_string(status_polls));
        RecordProperty(prefix + "_last_progress_sequence", std::to_string(last_progress_sequence));
        RecordProperty(prefix + "_max_ready_world_poll_us", std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(max_ready_world_poll).count()));
        RecordProperty(prefix + "_max_status_poll_us", std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(max_status_poll).count()));
        RecordProperty(prefix + "_max_polled_progress_observation_gap_us", std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(max_progress_observation_gap).count()));
        RecordProperty(prefix + "_terminal_elapsed_us", std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(
                (last_snapshot.state == server::WorldPreparationState::Ready
                    ? last_snapshot.state_changed_at : Clock::now()) - started_at).count()));
        RecordProperty(prefix + "_terminal_state", std::to_string(static_cast<unsigned>(last_snapshot.state)));
        EXPECT_EQ(last_snapshot.state, server::WorldPreparationState::Ready) << last_snapshot.error;
        EXPECT_EQ(terminal_ready_world != nullptr, last_snapshot.state == server::WorldPreparationState::Ready);
        if (last_snapshot.state == server::WorldPreparationState::Ready)
        {
            ASSERT_TRUE(last_snapshot.ready_world);
            ASSERT_TRUE(last_snapshot.ready_world->atlas());
            EXPECT_EQ(last_snapshot.ready_world->atlas()->configuration, config);

            Clock::time_point const l0_started_at = Clock::now();
            auto const l0_root = shared::buildTerrainLod(
                *last_snapshot.ready_world,
                shared::TerrainLodLevel::L0,
                0U,
                0U,
                WORLD_SIZE_BLOCKS
            );
            RecordProperty(prefix + "_client_l0_build_us", std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - l0_started_at).count()));
            ASSERT_TRUE(l0_root);

            client::S2WorldMapState const default_map_state{};
            client::S2WorldMapState map_state_without_river_geometry = default_map_state;
            map_state_without_river_geometry.layers[static_cast<size_t>(client::S2WorldMapLayer::Temperature)] = true;
            Clock::time_point const plain_map_raster_started_at = Clock::now();
            auto const plain_map_image = client::rasterizeS2WorldMap(
                *last_snapshot.ready_world,
                *l0_root,
                map_state_without_river_geometry
            );
            RecordProperty(prefix + "_client_map_raster_without_river_geometry_us", std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - plain_map_raster_started_at).count()));
            ASSERT_TRUE(plain_map_image);
            auto const record_map_fingerprint = [&](std::string_view const map_name,
                client::S2MapImage const& image) {
                std::string const property_prefix = prefix + "_" + std::string{map_name};
                RecordProperty(property_prefix + "_checksum", std::to_string(profileMapChecksum(image)));
                RecordProperty(property_prefix + "_flags", profileMapFlags(image));
                RecordProperty(property_prefix + "_dimensions",
                    std::to_string(image.width) + "x" + std::to_string(image.height));
            };
            record_map_fingerprint("map_without_river_geometry", *plain_map_image);

            Clock::time_point const map_raster_started_at = Clock::now();
            auto map_image = client::rasterizeS2WorldMap(
                *last_snapshot.ready_world,
                *l0_root,
                default_map_state
            );
            RecordProperty(prefix + "_client_map_raster_with_river_geometry_us", std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - map_raster_started_at).count()));
            ASSERT_TRUE(map_image);
            record_map_fingerprint("map_with_river_geometry", *map_image);

            Clock::time_point const cached_map_started_at = Clock::now();
            auto const cached_map_image = client::rasterizeS2WorldMap(
                *last_snapshot.ready_world,
                *l0_root,
                default_map_state
            );
            RecordProperty(prefix + "_client_map_raster_cached_geometry_us", std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - cached_map_started_at).count()));
            ASSERT_TRUE(cached_map_image);
            record_map_fingerprint("map_with_cached_river_geometry", *cached_map_image);
            EXPECT_EQ(profileMapChecksum(*map_image), profileMapChecksum(*cached_map_image));
            EXPECT_EQ(profileMapFlags(*map_image), profileMapFlags(*cached_map_image));

            auto const atlas = last_snapshot.ready_world->atlas();
            ASSERT_TRUE(atlas);
            auto const record_water_control = [&](std::string_view const control_name,
                double const x_m, double const y_m, bool const expected_ocean,
                bool const expected_lake, bool const expected_river) {
                V10SurfaceSample const sample = sampleV10AnnualMeanSurface(*atlas, x_m, y_m, 3U);
                EXPECT_EQ(sample.ocean, expected_ocean);
                EXPECT_EQ(sample.lake, expected_lake);
                EXPECT_EQ(sample.river, expected_river);
                EXPECT_DOUBLE_EQ(sample.elevation_m, v10L0ElevationAt(*atlas, x_m, y_m));
                std::string const property_prefix = prefix + "_" + std::string{control_name};
                RecordProperty(property_prefix + "_elevation_bits",
                    std::to_string(std::bit_cast<uint64_t>(sample.elevation_m)));
                RecordProperty(property_prefix + "_water_level_bits",
                    std::to_string(std::bit_cast<uint64_t>(sample.water_level_m)));
                RecordProperty(property_prefix + "_flags", std::string{
                    sample.ocean ? '1' : '0', sample.lake ? '1' : '0', sample.river ? '1' : '0'});
            };
            record_water_control("river_channel_control", 34'784.0, 30'212.0, false, false, true);
            record_water_control("river_adjacent_control", 34'784.0, 30'216.0, false, false, true);
            record_water_control("standing_lake_control", 43'520.0, 9'216.0, false, true, false);
            record_water_control("ocean_control", 34'860.0, 30'208.0, true, false, false);
            if (atlas->v10_provider && atlas->v10_provider->fine_terrain_cache)
            {
                auto const& cache = *atlas->v10_provider->fine_terrain_cache;
                RecordProperty(prefix + "_fine_kernel_initialized", cache.terrain ? "true" : "false");
                RecordProperty(prefix + "_fine_kernel_prepared_parent_queries",
                    std::to_string(cache.prepared_parent_queries));
            }

            auto const final_map = std::make_shared<client::S2MapImage const>(std::move(*map_image));
            ASSERT_TRUE(last_snapshot.latest_map_snapshot);
            client::GenerationMapAnimator animator;
            client::GenerationMapAnimationClock const animation_clock{
                .visible_elapsed = std::chrono::milliseconds{100},
                .reduced_motion = false,
                .ready = false,
                .formation_duration = std::chrono::seconds{10},
            };
            Clock::time_point const animation_started_at = Clock::now();
            auto const animation = animator.updateNative(
                last_snapshot.latest_map_snapshot,
                last_snapshot.ready_world->atlas(),
                final_map,
                animation_clock
            );
            RecordProperty(prefix + "_client_first_map_animation_us", std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - animation_started_at).count()));
            RecordProperty(prefix + "_client_first_map_animation_has_image", animation.image ? "true" : "false");
        }
    };

    run_profile(false, "reference");
    run_profile(true, "fine");
}
#endif
} // namespace
