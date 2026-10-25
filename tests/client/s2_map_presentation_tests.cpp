#include <client/GenerationMapPresentation.hpp>
#include <client/S2MapPresentation.hpp>

#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>
#include <generation/V9Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

std::shared_ptr<shared::TerrainWorld const> mapWorld(
    bool const complete_layers = true,
    bool const all_ocean = false
)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.resolution = 256U;
    atlas->vertical_scale = 10.0;
    uint32_t const count = 256U * 256U;
    atlas->elevation.assign(count, 1.0F);
    atlas->temperature.assign(count, 8.0F);
    atlas->precipitation.assign(count, 0.25F);
    atlas->water_level.assign(count, all_ocean ? 2.0F : 0.0F);
    atlas->ocean_mask.assign(count, all_ocean ? 1U : 0U);
    atlas->lake_mask.assign(count, 0U);
    atlas->river_mask.assign(count, 0U);
    if (complete_layers) {
        atlas->moisture.assign(count, 0.4F);
        atlas->wind_u.assign(count, 1.0F);
        atlas->wind_v.assign(count, 0.5F);
        atlas->current_u.assign(count, 0.25F);
        atlas->current_v.assign(count, -0.25F);
        atlas->climate_zone.assign(count, 3U);
        atlas->plate_id.assign(count, 1);
        atlas->plate_vx.assign(count, 0.1F);
        atlas->plate_vy.assign(count, 0.2F);
    }
    return std::make_shared<shared::TerrainWorld const>(std::move(atlas));
}

shared::TerrainLodArtifact localLod()
{
    shared::TerrainLodArtifact artifact;
    artifact.terrain_identity = shared::TerrainIdentity{
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = 1U,
        .seed = 42U,
        .configuration_identity = generation::configurationIdentity(generation::GenerationConfiguration{}),
    };
    artifact.level = shared::TerrainLodLevel::L1;
    artifact.spacing = 64U;
    artifact.origin_x = 0U;
    artifact.origin_y = 0U;
    artifact.extent = client::S2_MINIMAP_WORLD_EXTENT;
    artifact.side = artifact.extent / artifact.spacing + 1U;
    artifact.nodes.reserve(static_cast<size_t>(artifact.side) * artifact.side);
    for (uint32_t y = 0U; y < artifact.side; ++y) {
        for (uint32_t x = 0U; x < artifact.side; ++x) {
            shared::Block const material = x % 3U == 0U
                ? shared::Block::Grass
                : (x % 3U == 1U ? shared::Block::Stone : shared::Block::Snow);
            artifact.nodes.push_back({
                .key = {.x = x * artifact.spacing, .y = y * artifact.spacing},
                .sample = {
                    .height = 400.0 + static_cast<double>(x % 3U),
                    .top_material = material,
                },
            });
        }
    }
    return artifact;
}

} // namespace

TEST(S2MapPresentationTest, WorldRasterUsesOnlyFieldsPresentInTheMatchingAtlas)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, client::S2_WORLD_MAP_IMAGE_SIDE);
    EXPECT_EQ(image->rgba.size(), client::S2_WORLD_MAP_IMAGE_SIDE * client::S2_WORLD_MAP_IMAGE_SIDE);
    EXPECT_EQ(image->source_identity, world->identity());
    EXPECT_EQ(image->source_model_identity, shared::TERRAIN_LOD_MODEL);
    EXPECT_TRUE(image->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)]);
    EXPECT_TRUE(image->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Wind)]);
    EXPECT_TRUE(image->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)]);
    EXPECT_TRUE(image->has_numeric_range[static_cast<size_t>(client::S2WorldMapLayer::Plates)]);
    EXPECT_EQ(image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Plates)],
        (std::array<float, 2U>{1.0F, 1.0F}));
    EXPECT_TRUE(image->has_numeric_range[static_cast<size_t>(client::S2WorldMapLayer::Heights)]);
    EXPECT_EQ(image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Heights)],
        (std::array<float, 2U>{1.0F, 1.0F}));
    EXPECT_EQ(image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Temperature)],
        (std::array<float, 2U>{-40.0F, 30.0F}));
    EXPECT_EQ(image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Biomes)],
        (std::array<float, 2U>{0.0F, 5.0F}));
    EXPECT_EQ(image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Moisture)],
        (std::array<float, 2U>{0.4F, 0.4F}));
    EXPECT_EQ(image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Precipitation)],
        (std::array<float, 2U>{0.25F, 0.25F}));
    EXPECT_FALSE(image->has_numeric_range[static_cast<size_t>(client::S2WorldMapLayer::Wind)]);
    EXPECT_FALSE(image->has_numeric_range[static_cast<size_t>(client::S2WorldMapLayer::Currents)]);

    auto const partial_world = mapWorld(false);
    auto const partial_lod = shared::buildTerrainLod(*partial_world, shared::TerrainLodLevel::L0);
    auto const partial = client::rasterizeS2WorldMap(*partial_world, *partial_lod, state);
    ASSERT_TRUE(partial.has_value());
    EXPECT_FALSE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)]);
    EXPECT_FALSE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Wind)]);
    EXPECT_FALSE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)]);
    EXPECT_TRUE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Heights)]);
    EXPECT_TRUE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Temperature)]);
    EXPECT_FALSE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Biomes)]);
    EXPECT_FALSE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Moisture)]);
    EXPECT_TRUE(partial->available_layers[static_cast<size_t>(client::S2WorldMapLayer::Precipitation)]);
}

TEST(S2MapPresentationTest, V9WorldMapReadsAllEightNativeLayers)
{
    static constexpr size_t SOURCE_COUNT = 256U * 256U;
    static constexpr size_t CLIMATE_COUNT = 32U * 32U;
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V9;
    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr);

    auto const world = std::make_shared<shared::TerrainWorld const>(atlas, 17U);
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    auto const preview = client::buildS2MinimapPreview(*world, *lod, 35'456.0, 33'408.0);
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(preview->side, 65U);
    EXPECT_EQ(preview->terrain_identity, world->identity());
    EXPECT_EQ(preview->model_identity, shared::TERRAIN_LOD_MODEL_V9);
    ASSERT_TRUE(client::rasterizeS2Minimap(*preview));

    client::S2WorldMapState state;
    state.layers.fill(true);
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->source_identity, world->identity());
    EXPECT_EQ(image->source_model_identity, shared::TERRAIN_LOD_MODEL_V9);
    EXPECT_EQ(image->source_resolution, 256U);
    EXPECT_EQ(image->vector_field_resolution, 32U);
    for (bool const available : image->available_layers) {
        EXPECT_TRUE(available);
    }

    EXPECT_EQ(image->v9_plate_ids.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_elevation_m.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_temperature_mean_c.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_precipitation_mean_mm_year.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_biome_ids.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_moisture_mean_kg_m2.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v9_wind_u_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v9_wind_v_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v9_ocean_u_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v9_ocean_v_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->rgba.size(), client::S2_WORLD_MAP_IMAGE_SIDE * client::S2_WORLD_MAP_IMAGE_SIDE);

    auto const default_view = client::rasterizeS2WorldMap(*world, *lod, {});
    ASSERT_TRUE(default_view);
    static constexpr size_t UNFROZEN_SOURCE = 128U * 256U + 128U;
    EXPECT_EQ(default_view->v9_ice_surface_kind[UNFROZEN_SOURCE], 0U);
    EXPECT_EQ(default_view->rgba[256U * default_view->width + 256U],
        (std::array<uint8_t, 4U>{28U, 56U, 72U, 255U}));
    for (uint8_t const kind : {uint8_t{1U}, uint8_t{3U}}) {
        auto const found = std::find(default_view->v9_ice_surface_kind.begin(), default_view->v9_ice_surface_kind.end(), kind);
        ASSERT_NE(found, default_view->v9_ice_surface_kind.end());
        size_t const source_index = static_cast<size_t>(found - default_view->v9_ice_surface_kind.begin());
        size_t const pixel_index = source_index / 256U * 2U * default_view->width + source_index % 256U * 2U;
        auto const& frozen = default_view->rgba[pixel_index];
        EXPECT_GE(frozen[0U], kind == 3U ? 120U : 170U);
        EXPECT_GE(frozen[1U], kind == 3U ? 150U : 170U);
        EXPECT_GE(frozen[2U], 160U);
    }

    std::vector<generation::V9RegisteredFieldView> const fields = generation::v9RegisteredFields(*atlas);
    auto const findField = [&](std::string_view const name) {
        return std::find_if(fields.begin(), fields.end(), [name](auto const& field) {
            return field.name == name;
        });
    };
    auto const elevation = findField("elevation");
    auto const temperature = findField("temperature_C");
    auto const precipitation = findField("precipitation_mm_year");
    auto const biome = findField("biome_id");
    auto const moisture = findField("bucket_kg_m2");
    auto const wind_u = findField("u");
    auto const wind_v = findField("v");
    auto const current_u = findField("ocean_u");
    auto const current_v = findField("ocean_v");
    ASSERT_NE(elevation, fields.end());
    ASSERT_NE(temperature, fields.end());
    ASSERT_NE(precipitation, fields.end());
    ASSERT_NE(biome, fields.end());
    ASSERT_NE(moisture, fields.end());
    ASSERT_NE(wind_u, fields.end());
    ASSERT_NE(wind_v, fields.end());
    ASSERT_NE(current_u, fields.end());
    ASSERT_NE(current_v, fields.end());

    auto const fieldValue = [](generation::V9RegisteredFieldView const& field, size_t const index) {
        return std::visit([index](auto const values) {
            return static_cast<double>(values[index]);
        }, field.values);
    };
    auto const phaseMean = [&](generation::V9RegisteredFieldView const& field,
                               size_t const phase_size,
                               size_t const index) {
        double total = 0.0;
        for (size_t phase = 0U; phase < 8U; ++phase) {
            total += fieldValue(field, phase * phase_size + index);
        }
        return static_cast<float>(total / 8.0);
    };
    static constexpr std::array<size_t, 3U> SOURCE_PROBES{0U, 8'192U, SOURCE_COUNT - 1U};
    static constexpr std::array<size_t, 3U> CLIMATE_PROBES{0U, 512U, CLIMATE_COUNT - 1U};
    for (size_t const index : SOURCE_PROBES) {
        EXPECT_DOUBLE_EQ(image->v9_elevation_m[index], fieldValue(*elevation, index));
        EXPECT_FLOAT_EQ(image->v9_temperature_mean_c[index], phaseMean(*temperature, SOURCE_COUNT, index));
        EXPECT_FLOAT_EQ(image->v9_precipitation_mean_mm_year[index],
            phaseMean(*precipitation, SOURCE_COUNT, index));
        EXPECT_EQ(image->v9_biome_ids[index], static_cast<uint8_t>(fieldValue(*biome, index)));
    }
    for (size_t const index : CLIMATE_PROBES) {
        EXPECT_FLOAT_EQ(image->v9_moisture_mean_kg_m2[index], phaseMean(*moisture, CLIMATE_COUNT, index));
        EXPECT_FLOAT_EQ(image->v9_wind_u_m_s[index], phaseMean(*wind_u, CLIMATE_COUNT, index));
        EXPECT_FLOAT_EQ(image->v9_wind_v_m_s[index], phaseMean(*wind_v, CLIMATE_COUNT, index));
        EXPECT_FLOAT_EQ(image->v9_ocean_u_m_s[index], phaseMean(*current_u, CLIMATE_COUNT, index));
        EXPECT_FLOAT_EQ(image->v9_ocean_v_m_s[index], phaseMean(*current_v, CLIMATE_COUNT, index));
    }
}

TEST(S2MapPresentationTest, WorldMapUsesTheSharedCompletedAtlasBaseWhenLayersAreDisabled)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    auto const ice_mask = client::makeGenerationMapDisplayIceMask(
        std::span<uint8_t const>(world->atlas()->climate_zone),
        std::span<uint8_t const>(world->atlas()->ocean_mask),
        std::span<uint8_t const>(world->atlas()->lake_mask),
        std::span<float const>(world->atlas()->temperature)
    );
    ASSERT_TRUE(ice_mask.has_value());
    auto const shared_base = client::rasterizeGenerationMap(*world->atlas(), ice_mask->surface_kind);
    ASSERT_TRUE(shared_base.has_value());

    client::S2WorldMapState state;
    state.layers.fill(false);
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->rgba.front(), shared_base->rgba.front());
}

TEST(S2MapPresentationTest, TemperatureMoistureAndPrecipitationLayersUseOpaqueBoundedColors)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);

    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Temperature)] = true;
    auto const temperature = client::rasterizeS2WorldMap(*world, *lod, state);
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Moisture)] = true;
    auto const moisture = client::rasterizeS2WorldMap(*world, *lod, state);
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Precipitation)] = true;
    auto const precipitation = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(temperature);
    ASSERT_TRUE(moisture);
    ASSERT_TRUE(precipitation);
    EXPECT_EQ(temperature->rgba.front(), (std::array<uint8_t, 4U>{213U, 200U, 162U, 255U}));
    EXPECT_EQ(moisture->rgba.front(), (std::array<uint8_t, 4U>{209U, 189U, 137U, 255U}));
    EXPECT_EQ(precipitation->rgba.front(), (std::array<uint8_t, 4U>{220U, 213U, 235U, 255U}));
}

TEST(S2MapPresentationTest, MinimapUsesTheSuppliedLocalProjectionAndHeading)
{
    auto lod = localLod();
    auto const north_up = client::rasterizeS2Minimap(lod);
    auto const north = client::layoutS2MinimapMarker(lod, 16.0, 16.0, 0.0, {10.0F, 20.0F, 100.0F, 100.0F});
    auto const east = client::layoutS2MinimapMarker(lod, 16.0, 16.0, 90.0, {10.0F, 20.0F, 100.0F, 100.0F});

    ASSERT_TRUE(north_up.has_value());
    EXPECT_EQ(north_up->width, client::S2_MINIMAP_IMAGE_SIDE);
    EXPECT_EQ(north_up->source_identity, lod.terrain_identity);
    EXPECT_TRUE(north.visible);
    EXPECT_TRUE(east.visible);
    EXPECT_EQ(north.x, east.x);
    EXPECT_EQ(north.y, east.y);
    EXPECT_DOUBLE_EQ(north.heading_degrees, 0.0);
    EXPECT_DOUBLE_EQ(east.heading_degrees, 90.0);
}

TEST(S2MapPresentationTest, MinimapUsesWarmSandColorForSandSurfaceSamples)
{
    auto lod = localLod();
    for (shared::TerrainLodNode& node : lod.nodes) {
        node.sample.height = 400.0;
        node.sample.top_material = shared::Block::Sand;
        node.sample.water_present = false;
    }

    auto const image = client::rasterizeS2Minimap(lod);

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->rgba[image->width + 1U], (std::array<uint8_t, 4U>{211U, 194U, 149U, 255U}));
}

TEST(S2MapPresentationTest, MinimapPointerIsAHeadingRotatedFilledScreenShape)
{
    client::S2MinimapMarker north{.x = 60.0F, .y = 70.0F, .heading_degrees = 0.0, .visible = true};
    client::S2MinimapMarker east = north;
    east.heading_degrees = 90.0;
    std::vector<client::GuiVectorVertex> north_triangles;
    std::vector<client::GuiVectorVertex> east_triangles;

    client::appendS2MinimapPointerTriangles(north, {10.0F, 20.0F, 100.0F, 100.0F}, 1.0F, north_triangles);
    client::appendS2MinimapPointerTriangles(east, {10.0F, 20.0F, 100.0F, 100.0F}, 1.0F, east_triangles);

    ASSERT_EQ(north_triangles.size(), 78U);
    ASSERT_EQ(east_triangles.size(), north_triangles.size());
    EXPECT_NE(north_triangles.front().position, east_triangles.front().position);
    float north_tip_y = north_triangles.front().position[1U];
    float east_tip_x = east_triangles.front().position[0U];
    for (auto const& vertex : north_triangles) {
        north_tip_y = std::min(north_tip_y, vertex.position[1U]);
        EXPECT_TRUE(std::isfinite(vertex.position[0U]));
        EXPECT_TRUE(std::isfinite(vertex.position[1U]));
    }
    for (auto const& vertex : east_triangles) {
        east_tip_x = std::max(east_tip_x, vertex.position[0U]);
    }
    EXPECT_LT(north_tip_y, 62.0F);
    EXPECT_GT(north_tip_y, 59.0F);
    EXPECT_GT(east_tip_x, 68.0F);
    EXPECT_LT(east_tip_x, 71.0F);
    EXPECT_TRUE(std::all_of(north_triangles.end() - 6, north_triangles.end(), [](auto const& vertex) {
        return vertex.rgba == (std::array<uint8_t, 4U>{255U, 224U, 128U, 255U});
    }));
}

TEST(S2MapPresentationTest, WindAndCurrentArrowsAreScreenOverlaysNotRasterPixels)
{
    auto const world = mapWorld(true, true);
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState raster_state;
    auto const base = client::rasterizeS2WorldMap(*world, *lod, raster_state);
    raster_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Wind)] = true;
    raster_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)] = true;
    auto const vector_layers = client::rasterizeS2WorldMap(*world, *lod, raster_state);
    ASSERT_TRUE(base);
    ASSERT_TRUE(vector_layers);
    EXPECT_EQ(vector_layers->rgba, base->rgba);

    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    auto const overlay = client::buildS2WorldMapOverlay(
        raster_state, &*vector_layers, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, triangles);

    ASSERT_FALSE(triangles.empty());
    ASSERT_EQ(triangles.size() % 3U, 0U);
    EXPECT_LE(triangles.size(), client::MAXIMUM_GUI_VECTOR_VERTICES - 1U);
    EXPECT_TRUE(std::any_of(triangles.begin(), triangles.end(), [](auto const& vertex) {
        return vertex.rgba == (std::array<uint8_t, 4U>{99U, 219U, 212U, 255U});
    }));
    EXPECT_TRUE(std::any_of(triangles.begin(), triangles.end(), [](auto const& vertex) {
        return vertex.rgba == (std::array<uint8_t, 4U>{220U, 238U, 247U, 255U});
    }));
    for (auto const& vertex : triangles) {
        EXPECT_TRUE(std::isfinite(vertex.position[0U]));
        EXPECT_TRUE(std::isfinite(vertex.position[1U]));
        EXPECT_GE(vertex.position[0U], 0.0F);
        EXPECT_LT(vertex.position[0U], 1'280.0F);
        EXPECT_GE(vertex.position[1U], 0.0F);
        EXPECT_LT(vertex.position[1U], 676.0F);
        for (uint32_t panel_index = 0U; panel_index < overlay.panel_count; ++panel_index) {
            auto const& bounds = overlay.panels[panel_index].bounds;
            EXPECT_FALSE(vertex.position[0U] >= bounds[0U] && vertex.position[0U] < bounds[0U] + bounds[2U]
                && vertex.position[1U] >= bounds[1U] && vertex.position[1U] < bounds[1U] + bounds[3U]);
        }
    }
}

TEST(S2MapPresentationTest, PlateMotionAndNamesUseTheScreenOverlayBatch)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    ASSERT_EQ(image->plate_motions.size(), 1U);

    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(
        state, &*image, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, triangles));

    EXPECT_FALSE(triangles.empty());
    EXPECT_TRUE(std::any_of(labels.labels().begin(), labels.labels().end(), [](client::TextLabel const& label) {
        return label.value == "P1";
    }));
}

TEST(S2MapPresentationTest, MinimapLocalLodStitchesAcrossBothWorldWrapSeams)
{
    auto const world = mapWorld();
    auto const lod = client::buildS2MinimapLod(*world, 0.0, 0.0);

    ASSERT_NE(lod, nullptr);
    EXPECT_EQ(lod->level, shared::TerrainLodLevel::L1);
    EXPECT_EQ(lod->extent, client::S2_MINIMAP_WORLD_EXTENT);
    EXPECT_EQ(lod->side, client::S2_MINIMAP_WORLD_EXTENT / lod->spacing + 1U);
    EXPECT_LE(lod->side, client::S2_MINIMAP_IMAGE_SIDE * 2U + 1U);
    EXPECT_LE(lod->nodes.size(), shared::TERRAIN_LOD_MAX_NODES);
    EXPECT_EQ(lod->origin_x, generation::WORLD_SIZE_BLOCKS - client::S2_MINIMAP_WORLD_EXTENT / 2U);
    EXPECT_EQ(lod->origin_y, generation::WORLD_SIZE_BLOCKS - client::S2_MINIMAP_WORLD_EXTENT / 2U);
    uint32_t const seam_x = (generation::WORLD_SIZE_BLOCKS - lod->origin_x) / lod->spacing;
    uint32_t const seam_y = (generation::WORLD_SIZE_BLOCKS - lod->origin_y) / lod->spacing;
    EXPECT_EQ(lod->nodes[seam_x].key.x, 0U);
    EXPECT_EQ(lod->nodes[seam_x + 1U].key.x, lod->spacing);
    EXPECT_EQ(lod->nodes[static_cast<size_t>(seam_y) * lod->side].key.y, 0U);
    EXPECT_EQ(lod->nodes[static_cast<size_t>(seam_y + 1U) * lod->side].key.y, lod->spacing);
    EXPECT_TRUE(client::rasterizeS2Minimap(*lod).has_value());
}

TEST(S2MapPresentationTest, MapLayoutWrapsTheCloseViewAndFitsOneUnrepeatedOverview)
{
    client::S2WorldMapState state{
        .center_x = 65'530.0,
        .center_y = 2.0,
        .zoom = 2.0,
    };
    client::S2MapLayout const close = client::layoutS2Map(state, 1'200U, 800U, true, false);
    EXPECT_GT(close.world_draw_count, 1U);
    EXPECT_LE(close.world_draw_count, close.world_draws.size());

    state.overview = true;
    client::S2MapLayout const overview = client::layoutS2Map(state, 1'200U, 800U, true, true);
    EXPECT_EQ(overview.world_draw_count, 1U);
    EXPECT_FALSE(overview.show_minimap);
    EXPECT_NEAR(overview.world_draws[0].destination[2], overview.world_draws[0].destination[3], 0.001F);

    client::S2MapLayout const minimap = client::layoutS2Map({}, 1'280U, 676U, false, true);
    ASSERT_TRUE(minimap.show_minimap);
    EXPECT_EQ(minimap.minimap_bounds, (std::array<float, 4U>{1'054.0F, 44.0F, 192.0F, 192.0F}));

    client::S2MapLayout const hidpi_minimap = client::layoutS2Map(
        {}, 2'560U, 1'352U, false, true, 2.0F);
    ASSERT_TRUE(hidpi_minimap.show_minimap);
    EXPECT_EQ(hidpi_minimap.minimap_bounds,
        (std::array<float, 4U>{2'108.0F, 88.0F, 384.0F, 384.0F}));
}

TEST(S2MapPresentationTest, FullPeriodHorizontalViewPreservesWrappedPanOffset)
{
    client::S2WorldMapState state{
        .center_x = static_cast<double>(generation::WORLD_SIZE_BLOCKS) * 0.25,
        .center_y = static_cast<double>(generation::WORLD_SIZE_BLOCKS) * 0.5,
        .zoom = 1.0,
    };

    client::S2MapLayout const layout = client::layoutS2Map(state, 1'200U, 800U, true, false);

    ASSERT_EQ(layout.world_draw_count, 2U);
    EXPECT_FLOAT_EQ(layout.world_draws[0].source[0], 0.75F);
    EXPECT_FLOAT_EQ(layout.world_draws[0].source[2], 0.25F);
    EXPECT_FLOAT_EQ(layout.world_draws[0].destination[0], 0.0F);
    EXPECT_FLOAT_EQ(layout.world_draws[0].destination[2], 300.0F);
    EXPECT_FLOAT_EQ(layout.world_draws[1].source[0], 0.0F);
    EXPECT_FLOAT_EQ(layout.world_draws[1].source[2], 0.75F);
    EXPECT_FLOAT_EQ(layout.world_draws[1].destination[0], 300.0F);
    EXPECT_FLOAT_EQ(layout.world_draws[1].destination[2], 900.0F);
}

TEST(S2MapPresentationTest, WorldMapViewComposesWrappedRasterAndOpaqueCanvas)
{
    client::S2MapImage source;
    source.width = 4U;
    source.height = 2U;
    source.rgba = {
        {10U, 20U, 30U, 255U}, {60U, 50U, 50U, 255U},
        {110U, 80U, 70U, 255U}, {160U, 110U, 90U, 255U},
        {11U, 21U, 31U, 255U}, {61U, 51U, 51U, 255U},
        {111U, 81U, 71U, 255U}, {161U, 111U, 91U, 255U},
    };
    client::S2MapLayout layout;
    layout.world_draw_count = 2U;
    layout.world_draws[0] = {
        .image = 0U,
        .source = {0.5F, 0.0F, 0.5F, 1.0F},
        .destination = {0.0F, 0.0F, 2.0F, 2.0F},
        .scissor = {0U, 0U, 4U, 2U},
    };
    layout.world_draws[1] = {
        .image = 0U,
        .source = {0.0F, 0.0F, 0.5F, 1.0F},
        .destination = {2.0F, 0.0F, 2.0F, 2.0F},
        .scissor = {0U, 0U, 4U, 2U},
    };
    client::S2MapOverlayLayout overlay;
    overlay.panel_count = 1U;
    overlay.panels[0U] = { .bounds = {0.0F, 0.0F, 1.0F, 2.0F}, .palette_index = 1U };

    auto const view = client::composeS2WorldMapView(&source, layout, overlay, 4U, 2U);

    ASSERT_TRUE(view);
    EXPECT_EQ(view->width, 4U);
    EXPECT_EQ(view->height, 2U);
    EXPECT_EQ(view->rgba[0U], (std::array<uint8_t, 4U>{49U, 48U, 49U, 255U}));
    EXPECT_EQ(view->rgba[1U], (std::array<uint8_t, 4U>{160U, 110U, 90U, 255U}));
    EXPECT_EQ(view->rgba[2U], (std::array<uint8_t, 4U>{10U, 20U, 30U, 255U}));
    EXPECT_EQ(view->rgba[3U], (std::array<uint8_t, 4U>{60U, 50U, 50U, 255U}));
    EXPECT_TRUE(std::all_of(view->rgba.begin(), view->rgba.end(), [](auto const& pixel) {
        return pixel[3U] == 255U;
    }));

    auto const canvas = client::composeS2WorldMapView(nullptr, {}, {}, 600U, 300U);
    ASSERT_TRUE(canvas);
    EXPECT_EQ(canvas->width, 512U);
    EXPECT_EQ(canvas->height, 256U);
    EXPECT_TRUE(std::all_of(canvas->rgba.begin(), canvas->rgba.end(), [](auto const& pixel) {
        return pixel == (std::array<uint8_t, 4U>{21U, 39U, 47U, 255U});
    }));
}

TEST(S2MapPresentationTest, LayerAndNavigationActionsAreIndependentAndBounded)
{
    client::S2WorldMapState state;
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::ToggleWind, 2'000.0, 4'000.0));
    EXPECT_TRUE(state.layers[static_cast<size_t>(client::S2WorldMapLayer::Wind)]);
    EXPECT_FALSE(state.layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)]);

    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::PanLeft, 2'000.0, 4'000.0, true));
    EXPECT_GE(state.center_x, 0.0);
    EXPECT_LT(state.center_x, generation::WORLD_SIZE_BLOCKS);
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::Overview, 2'000.0, 4'000.0));
    EXPECT_TRUE(state.overview);
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::ZoomIn, 2'000.0, 4'000.0));
    EXPECT_FALSE(state.overview);
    EXPECT_GT(state.zoom, 1.0);
}
TEST(S2MapPresentationTest, CancelledMinimapRequestReturnsNoResultToWorker)
{
    auto const world = mapWorld();
    std::stop_source cancellation;
    cancellation.request_stop();
    std::shared_ptr<shared::TerrainLodArtifact const> result;
    std::thread worker{[&] {
        EXPECT_NO_THROW(result = client::buildS2MinimapLod(*world, 0.0, 0.0, cancellation.get_token()));
    }};
    worker.join();
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(client::buildS2MinimapLod(*world, 0.0, 0.0), nullptr);
}

TEST(S2MapPresentationTest, InitialMinimapUsesExactRootNodesAcrossWrapAndRejectsOtherSources)
{
    auto const world = mapWorld();
    auto root = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    auto preview = client::buildS2MinimapPreview(*world, *root, 0.0, 0.0);
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(preview->level, shared::TerrainLodLevel::L0);
    EXPECT_EQ(preview->terrain_identity, root->terrain_identity);
    EXPECT_EQ(preview->model_identity, root->model_identity);
    EXPECT_EQ(preview->extent, client::S2_MINIMAP_WORLD_EXTENT);
    EXPECT_EQ(preview->spacing, root->spacing);
    for (auto const& node : preview->nodes) {
        auto const index = static_cast<size_t>(node.key.y / root->spacing) * root->side
            + node.key.x / root->spacing;
        EXPECT_EQ(node, root->nodes[index]);
    }
    auto const image = client::rasterizeS2Minimap(*preview);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->source_identity, world->identity());
    EXPECT_TRUE(client::layoutS2MinimapMarker(
        *preview, 0.0, 0.0, 0.0, {0.0F, 0.0F, 256.0F, 256.0F}).visible);
    auto mismatched = *root;
    ++mismatched.terrain_identity.revision;
    EXPECT_EQ(client::buildS2MinimapPreview(*world, mismatched, 0.0, 0.0), nullptr);
}
