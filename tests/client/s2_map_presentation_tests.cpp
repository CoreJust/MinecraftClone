#include <client/Camera.hpp>
#include <client/GenerationMapPresentation.hpp>
#include <client/S2MapPresentation.hpp>
#include <client/render/CanonicalTerrainField.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>
#include <generation/V10Provider.hpp>
#include <generation/V9Provider.hpp>

#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    static constexpr uint32_t SOURCE_RESOLUTION = 256U;
    static constexpr uint32_t CLIMATE_RESOLUTION = 32U;
    static constexpr size_t SOURCE_COUNT = SOURCE_RESOLUTION * SOURCE_RESOLUTION;
    static constexpr size_t CLIMATE_COUNT = CLIMATE_RESOLUTION * CLIMATE_RESOLUTION;
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
    EXPECT_EQ(image->ocean_mask_resolution, SOURCE_RESOLUTION);
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
    EXPECT_EQ(image->v9_ocean_cells.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_ice_surface_kind.size(), SOURCE_COUNT);
    EXPECT_EQ(image->rgba.size(), client::S2_WORLD_MAP_IMAGE_SIDE * client::S2_WORLD_MAP_IMAGE_SIDE);

    auto const default_view = client::rasterizeS2WorldMap(*world, *lod, {});
    ASSERT_TRUE(default_view);
    static constexpr size_t UNFROZEN_SOURCE = 128U * 256U + 128U;
    EXPECT_EQ(default_view->v9_ice_surface_kind[UNFROZEN_SOURCE], 0U);
    // This seed's sample is below sea level. Muting inland water must not
    // replace the established deep-ocean depth palette.
    ASSERT_LT(default_view->v9_elevation_m[UNFROZEN_SOURCE], 0.0);
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
    client::S2WorldMapState heights_only_state;
    heights_only_state.layers.fill(false);
    heights_only_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Heights)] = true;
    auto const heights_only = client::rasterizeS2WorldMap(*world, *lod, heights_only_state);
    heights_only_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Biomes)] = true;
    auto const heights_and_biomes = client::rasterizeS2WorldMap(*world, *lod, heights_only_state);
    ASSERT_TRUE(heights_only);
    ASSERT_TRUE(heights_and_biomes);
    EXPECT_NE(heights_and_biomes->rgba, heights_only->rgba);

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
    auto const ocean = findField("ocean");
    auto const lake = findField("lake");
    auto const river = findField("river");
    ASSERT_NE(elevation, fields.end());
    ASSERT_NE(temperature, fields.end());
    ASSERT_NE(precipitation, fields.end());
    ASSERT_NE(biome, fields.end());
    ASSERT_NE(moisture, fields.end());
    ASSERT_NE(wind_u, fields.end());
    ASSERT_NE(wind_v, fields.end());
    ASSERT_NE(current_u, fields.end());
    ASSERT_NE(current_v, fields.end());
    ASSERT_NE(ocean, fields.end());
    ASSERT_NE(lake, fields.end());
    ASSERT_NE(river, fields.end());

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

    auto const elevation_values = std::get<std::span<double const>>(elevation->values);
    auto const temperature_values = std::get<std::span<float const>>(temperature->values);
    auto const biome_values = std::get<std::span<uint8_t const>>(biome->values);
    auto const ocean_values = std::get<std::span<uint8_t const>>(ocean->values);
    ASSERT_EQ(ocean_values.size(), SOURCE_COUNT);
    for (size_t index = 0U; index < SOURCE_COUNT; ++index) {
        EXPECT_EQ(image->v9_ocean_cells[index], ocean_values[index]);
    }
    auto const lake_values = std::get<std::span<uint8_t const>>(lake->values);
    auto const river_values = std::get<std::span<uint8_t const>>(river->values);
    auto const hasWater = [&](uint32_t const x, uint32_t const y) {
        size_t const index = static_cast<size_t>(y) * SOURCE_RESOLUTION + x;
        return ocean_values[index] != 0U || lake_values[index] != 0U || river_values[index] != 0U;
    };
    auto const hasAdjacentOceanOrLake = [&](uint32_t const x, uint32_t const y) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                uint32_t const neighbor_x = static_cast<uint32_t>(
                    (static_cast<int32_t>(x) + dx + static_cast<int32_t>(SOURCE_RESOLUTION))
                    % static_cast<int32_t>(SOURCE_RESOLUTION)
                );
                uint32_t const neighbor_y = static_cast<uint32_t>(
                    (static_cast<int32_t>(y) + dy + static_cast<int32_t>(SOURCE_RESOLUTION))
                    % static_cast<int32_t>(SOURCE_RESOLUTION)
                );
                size_t const neighbor = static_cast<size_t>(neighbor_y) * SOURCE_RESOLUTION + neighbor_x;
                if (ocean_values[neighbor] != 0U || lake_values[neighbor] != 0U) {
                    return true;
                }
            }
        }
        return false;
    };
    auto const meanTemperatureAt = [&](size_t const index) {
        double result = 0.0;
        for (size_t phase = 0U; phase < 8U; ++phase) {
            result += temperature_values[phase * SOURCE_COUNT + index];
        }
        return result / 8.0;
    };
    auto const findExposedCell = [&](auto const& eligible)
        -> std::optional<std::array<uint32_t, 2U>> {
        for (uint32_t y = 0U; y < SOURCE_RESOLUTION; ++y) {
            for (uint32_t x = 0U; x < SOURCE_RESOLUTION; ++x) {
                size_t const index = static_cast<size_t>(y) * SOURCE_RESOLUTION + x;
                if (!eligible(index, x, y)) {
                    continue;
                }
                uint32_t const world_x = x * (generation::WORLD_SIZE_BLOCKS / SOURCE_RESOLUTION);
                uint32_t const world_y = y * (generation::WORLD_SIZE_BLOCKS / SOURCE_RESOLUTION);
                shared::SurfaceTile const surface = world->sampleSurface({
                    .x = static_cast<int32_t>(world_x / shared::Chunk::SIDE_LENGTH),
                    .y = static_cast<int32_t>(world_y / shared::Chunk::SIDE_LENGTH),
                    .z = 0,
                });
                shared::TerrainColumn const* const column = surface.columnAt({.x = 0U, .y = 0U});
                if (column == nullptr || column->submerged || column->water_present) {
                    continue;
                }
                return std::array<uint32_t, 2U>{x, y};
            }
        }
        return std::nullopt;
    };
    auto const desert_cell = findExposedCell([&](size_t const index, uint32_t const x, uint32_t const y) {
        return biome_values[index] == 5U && elevation_values[index] > 0.0 && !hasWater(x, y);
    });
    auto const summit_cell = findExposedCell([&](size_t const index, uint32_t const x, uint32_t const y) {
        return biome_values[index] == 0U && meanTemperatureAt(index) < 0.0
            && elevation_values[index] > 0.0 && !hasWater(x, y)
            && !hasAdjacentOceanOrLake(x, y);
    });
    auto const coast_cell = findExposedCell([&](size_t const index, uint32_t const x, uint32_t const y) {
        return biome_values[index] != 0U && biome_values[index] != 5U
            && elevation_values[index] > 0.0 && elevation_values[index] <= 20.0
            && !hasWater(x, y) && hasAdjacentOceanOrLake(x, y);
    });
    ASSERT_TRUE(desert_cell.has_value()) << "seed 42 must retain an exposed hot-desert source cell";
    ASSERT_TRUE(summit_cell.has_value()) << "seed 42 must retain an exposed sub-zero summit source cell";
    ASSERT_TRUE(coast_cell.has_value()) << "seed 42 must retain an exposed low coastal source cell";

    std::vector<client::render::CanonicalTerrainFieldValue> const canonical_render_fields =
        client::render::buildV9CanonicalTerrainFieldValues(*atlas);
    ASSERT_EQ(canonical_render_fields.size(), SOURCE_COUNT);
    auto const expectCanonicalRenderMaterial = [&](std::array<uint32_t, 2U> const cell, shared::Block const material) {
        size_t const index = static_cast<size_t>(cell[1U]) * SOURCE_RESOLUTION + cell[0U];
        EXPECT_EQ(static_cast<uint8_t>(canonical_render_fields[index][2U]), static_cast<uint8_t>(material));
    };
    expectCanonicalRenderMaterial(*desert_cell, shared::Block::Sand);
    expectCanonicalRenderMaterial(*coast_cell, shared::Block::Sand);
    expectCanonicalRenderMaterial(*summit_cell, shared::Block::Snow);

    client::S2MapImage coast_image = *image;
    coast_image.v9_ocean_u_m_s.assign(CLIMATE_COUNT, 1.0F);
    coast_image.v9_ocean_v_m_s.assign(CLIMATE_COUNT, 0.0F);
    coast_image.v9_ocean_cells.assign(SOURCE_COUNT, 1U);
    static constexpr uint32_t ISLAND_X_FIRST = 126U;
    static constexpr uint32_t ISLAND_X_END = 128U;
    static constexpr uint32_t ISLAND_Y_FIRST = 80U;
    static constexpr uint32_t ISLAND_Y_END = 176U;
    for (uint32_t y = ISLAND_Y_FIRST; y < ISLAND_Y_END; ++y) {
        for (uint32_t x = ISLAND_X_FIRST; x < ISLAND_X_END; ++x) {
            coast_image.v9_ocean_cells[static_cast<size_t>(y) * SOURCE_RESOLUTION + x] = 0U;
        }
    }
    EXPECT_EQ(coast_image.v9_ocean_cells[
        static_cast<size_t>(ISLAND_Y_FIRST) * SOURCE_RESOLUTION + ISLAND_X_FIRST], 0U);
    EXPECT_LT(ISLAND_X_END - ISLAND_X_FIRST, 8U);
    bool coarse_climate_centers_are_ocean = true;
    for (uint32_t y = 0U; y < CLIMATE_RESOLUTION; ++y) {
        for (uint32_t x = 0U; x < CLIMATE_RESOLUTION; ++x) {
            size_t const source_index = static_cast<size_t>(y * 8U + 4U) * SOURCE_RESOLUTION + x * 8U + 4U;
            coarse_climate_centers_are_ocean = coarse_climate_centers_are_ocean
                && coast_image.v9_ocean_cells[source_index] != 0U;
        }
    }
    EXPECT_TRUE(coarse_climate_centers_are_ocean);

    client::S2WorldMapState current_state;
    current_state.layers.fill(false);
    current_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)] = true;
    client::TextRenderer current_labels;
    std::vector<client::GuiVectorVertex> current_triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(current_state, &coast_image, world.get(),
        32'768.0, 32'768.0, 1'280U, 676U, 1.0F, current_labels, current_triangles, 0.37F));
    client::S2MapLayout const current_layout = client::layoutS2Map(
        current_state, 1'280U, 676U, true, false, 1.0F);
    size_t current_geometry_vertices = 0U;
    for (size_t triangle = 0U; triangle + 2U < current_triangles.size(); triangle += 3U) {
        auto const& a = current_triangles[triangle];
        auto const& b = current_triangles[triangle + 1U];
        auto const& c = current_triangles[triangle + 2U];
        bool const current_color = a.rgba[0U] == 162U && a.rgba[1U] == 224U && a.rgba[2U] == 249U;
        bool const outline_color = a.rgba[0U] == 17U && a.rgba[1U] == 42U && a.rgba[2U] == 53U;
        if (!current_color && !outline_color) {
            continue;
        }
        current_geometry_vertices += 3U;
        auto const sourcePoint = [&](client::GuiVectorVertex const& vertex)
            -> std::optional<std::array<float, 2U>> {
            for (uint32_t draw_index = 0U; draw_index < current_layout.world_draw_count; ++draw_index) {
                client::MapImageDraw const& draw = current_layout.world_draws[draw_index];
                if (vertex.position[0U] < draw.destination[0U]
                    || vertex.position[0U] > draw.destination[0U] + draw.destination[2U]
                    || vertex.position[1U] < draw.destination[1U]
                    || vertex.position[1U] > draw.destination[1U] + draw.destination[3U]) {
                    continue;
                }
                return std::array<float, 2U>{
                    draw.source[0U] + (vertex.position[0U] - draw.destination[0U])
                        / draw.destination[2U] * draw.source[2U],
                    draw.source[1U] + (vertex.position[1U] - draw.destination[1U])
                        / draw.destination[3U] * draw.source[3U],
                };
            }
            return std::nullopt;
        };
        auto const source_a = sourcePoint(a);
        auto const source_b = sourcePoint(b);
        auto const source_c = sourcePoint(c);
        ASSERT_TRUE(source_a);
        ASSERT_TRUE(source_b);
        ASSERT_TRUE(source_c);
        float const min_x = std::min({(*source_a)[0U], (*source_b)[0U], (*source_c)[0U]});
        float const max_x = std::max({(*source_a)[0U], (*source_b)[0U], (*source_c)[0U]});
        float const min_y = std::min({(*source_a)[1U], (*source_b)[1U], (*source_c)[1U]});
        float const max_y = std::max({(*source_a)[1U], (*source_b)[1U], (*source_c)[1U]});
        uint32_t const first_x = std::min(static_cast<uint32_t>(std::floor(min_x * SOURCE_RESOLUTION)),
            SOURCE_RESOLUTION - 1U);
        uint32_t const last_x = std::min(static_cast<uint32_t>(std::floor(max_x * SOURCE_RESOLUTION)),
            SOURCE_RESOLUTION - 1U);
        uint32_t const first_y = std::min(static_cast<uint32_t>(std::floor(min_y * SOURCE_RESOLUTION)),
            SOURCE_RESOLUTION - 1U);
        uint32_t const last_y = std::min(static_cast<uint32_t>(std::floor(max_y * SOURCE_RESOLUTION)),
            SOURCE_RESOLUTION - 1U);
        for (uint32_t y = first_y; y <= last_y; ++y) {
            for (uint32_t x = first_x; x <= last_x; ++x) {
                EXPECT_NE(coast_image.v9_ocean_cells[static_cast<size_t>(y) * SOURCE_RESOLUTION + x], 0U)
                    << "current triangle crosses the fine island at " << x << "," << y;
            }
        }
    }
    EXPECT_GT(current_geometry_vertices, 0U);
}

TEST(S2MapPresentationTest, V21WorldMapUsesNativePhaseFieldsAndStockIce)
{
    static constexpr uint32_t SOURCE_RESOLUTION = 256U;
    static constexpr uint32_t CLIMATE_RESOLUTION = 32U;
    static constexpr size_t SOURCE_COUNT = static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION;
    static constexpr size_t CLIMATE_COUNT = static_cast<size_t>(CLIMATE_RESOLUTION) * CLIMATE_RESOLUTION;
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V21;
    auto const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr);
    EXPECT_TRUE(atlas->elevation.empty());
    EXPECT_TRUE(atlas->temperature.empty());
    EXPECT_TRUE(atlas->plate_id.empty());

    auto const world = std::make_shared<shared::TerrainWorld const>(atlas, 17U);
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    EXPECT_EQ(lod->model_identity, shared::terrainLodModel(generation::TerrainModelTag::V21));
    client::S2WorldMapState state;
    state.layers.fill(true);
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->source_identity, world->identity());
    EXPECT_EQ(image->source_model_identity, lod->model_identity);
    EXPECT_EQ(image->source_resolution, SOURCE_RESOLUTION);
    EXPECT_EQ(image->vector_field_resolution, CLIMATE_RESOLUTION);
    EXPECT_EQ(image->ocean_mask_resolution, SOURCE_RESOLUTION);
    for (bool const available : image->available_layers)
    {
        EXPECT_TRUE(available);
    }
    EXPECT_TRUE(image->v9_plate_ids.empty());
    EXPECT_TRUE(image->v9_elevation_m.empty());
    EXPECT_EQ(image->v21_plate_ids.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v21_elevation_m.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v21_temperature_mean_c.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v21_precipitation_mean_mm_year.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v21_biome_ids.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v21_moisture_mean_kg_m2.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v21_wind_u_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v21_wind_v_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v21_ocean_u_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v21_ocean_v_m_s.size(), CLIMATE_COUNT);
    EXPECT_EQ(image->v21_ocean_cells.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v21_ice_surface_kind.size(), SOURCE_COUNT);

    auto const fields = generation::v21RegisteredFields(*atlas);
    auto const find_field = [&](std::string_view const name) {
        return std::find_if(fields.begin(), fields.end(), [name](auto const& field) {
            return field.name == name;
        });
    };
    auto const elevation = find_field("elevation");
    auto const temperature = find_field("temperature_C");
    auto const precipitation = find_field("precipitation_mm_year");
    auto const biome = find_field("biome_id");
    auto const ocean = find_field("ocean");
    auto const lake = find_field("lake");
    auto const river = find_field("river");
    auto const snow = find_field("land_snow_water_equivalent_m");
    auto const glacier = find_field("glacier_thickness_m");
    auto const sea_ice = find_field("sea_ice_thickness_m");
    ASSERT_NE(elevation, fields.end());
    ASSERT_NE(temperature, fields.end());
    ASSERT_NE(precipitation, fields.end());
    ASSERT_NE(biome, fields.end());
    ASSERT_NE(ocean, fields.end());
    ASSERT_NE(lake, fields.end());
    ASSERT_NE(river, fields.end());
    ASSERT_NE(snow, fields.end());
    ASSERT_NE(glacier, fields.end());
    ASSERT_NE(sea_ice, fields.end());
    auto const value_at = [](generation::V9RegisteredFieldView const& field, size_t const index) {
        return std::visit([index](auto const values) {
            return static_cast<double>(values[index]);
        }, field.values);
    };
    for (size_t index = 0U; index < SOURCE_COUNT; ++index)
    {
        EXPECT_EQ(image->v21_ocean_cells[index], value_at(*ocean, index) != 0.0 ? 1U : 0U);
    }
    for (size_t index = 0U; index < SOURCE_COUNT; ++index)
    {
        EXPECT_EQ(image->v21_ocean_cells[index], value_at(*ocean, index) != 0.0 ? 1U : 0U);
    }
    auto const phase_mean = [&](
        generation::V9RegisteredFieldView const& field,
        size_t const field_size,
        size_t const index
    ) {
        double total = 0.0;
        for (size_t phase = 0U; phase < 8U; ++phase)
        {
            total += value_at(field, phase * field_size + index);
        }
        return total / 8.0;
    };
    for (size_t const index : {size_t{0U}, size_t{8'192U}, SOURCE_COUNT - 1U})
    {
        EXPECT_DOUBLE_EQ(image->v21_elevation_m[index], value_at(*elevation, index));
        EXPECT_FLOAT_EQ(image->v21_temperature_mean_c[index], phase_mean(*temperature, SOURCE_COUNT, index));
        EXPECT_FLOAT_EQ(image->v21_precipitation_mean_mm_year[index],
            phase_mean(*precipitation, SOURCE_COUNT, index));
        EXPECT_EQ(image->v21_biome_ids[index], static_cast<uint8_t>(value_at(*biome, index)));
    }

    static constexpr float DISPLAY_ICE_CUTOFF_M = 0.1F;
    ASSERT_EQ(lod->side, SOURCE_RESOLUTION + 1U);
    ASSERT_EQ(lod->nodes.size(), static_cast<size_t>(lod->side) * lod->side);
    bool has_phase_zero_snow = false;
    bool has_phase_zero_glacier = false;
    bool has_phase_zero_sea_ice = false;
    bool phase_zero_differs_from_mean = false;
    for (size_t index = 0U; index < SOURCE_COUNT; ++index)
    {
        bool const water = value_at(*ocean, index) != 0.0 || value_at(*lake, index) != 0.0
            || value_at(*river, index) != 0.0;
        double const snow_phase_zero = value_at(*snow, index);
        double const glacier_phase_zero = value_at(*glacier, index);
        double const sea_ice_phase_zero = value_at(*sea_ice, index);
        bool const snow_present = !water && snow_phase_zero >= DISPLAY_ICE_CUTOFF_M;
        bool const glacier_present = !water && glacier_phase_zero >= DISPLAY_ICE_CUTOFF_M;
        bool const sea_ice_present = value_at(*ocean, index) != 0.0
            && sea_ice_phase_zero >= DISPLAY_ICE_CUTOFF_M;
        uint8_t const expected_kind = sea_ice_present ? 3U
            : (snow_present ? 1U : (glacier_present ? 2U : 0U));
        EXPECT_EQ(image->v21_ice_surface_kind[index], expected_kind);
        has_phase_zero_snow = has_phase_zero_snow || snow_present;
        has_phase_zero_glacier = has_phase_zero_glacier || glacier_present;
        has_phase_zero_sea_ice = has_phase_zero_sea_ice || sea_ice_present;
        phase_zero_differs_from_mean = phase_zero_differs_from_mean
            || ((snow_phase_zero >= DISPLAY_ICE_CUTOFF_M)
                != (phase_mean(*snow, SOURCE_COUNT, index) >= DISPLAY_ICE_CUTOFF_M))
            || ((glacier_phase_zero >= DISPLAY_ICE_CUTOFF_M)
                != (phase_mean(*glacier, SOURCE_COUNT, index) >= DISPLAY_ICE_CUTOFF_M))
            || ((sea_ice_phase_zero >= DISPLAY_ICE_CUTOFF_M)
                != (phase_mean(*sea_ice, SOURCE_COUNT, index) >= DISPLAY_ICE_CUTOFF_M));
        size_t const lod_index = (index / SOURCE_RESOLUTION) * lod->side + index % SOURCE_RESOLUTION;
        shared::Block const world_material = lod->nodes[lod_index].sample.top_material;
        if (expected_kind == 1U)
        {
            EXPECT_EQ(world_material, shared::Block::Snow);
        }
        else if (expected_kind == 2U)
        {
            EXPECT_EQ(world_material, shared::Block::Ice);
        }
    }
    EXPECT_TRUE(has_phase_zero_snow);
    EXPECT_TRUE(has_phase_zero_glacier);
    EXPECT_TRUE(has_phase_zero_sea_ice);
    EXPECT_TRUE(phase_zero_differs_from_mean);

    auto const canonical_fields = client::render::buildV21CanonicalTerrainFieldValues(*atlas);
    ASSERT_EQ(canonical_fields.size(), SOURCE_COUNT);
    ASSERT_EQ(lod->spacing, 256U);
    for (uint32_t y = 0U; y < SOURCE_RESOLUTION; ++y)
    {
        for (uint32_t x = 0U; x < SOURCE_RESOLUTION; ++x)
        {
            size_t const source_index = static_cast<size_t>(y) * SOURCE_RESOLUTION + x;
            size_t const lod_index = static_cast<size_t>(y) * lod->side + x;
            shared::TerrainLodNode const& node = lod->nodes[lod_index];
            EXPECT_EQ(node.key.x, x * lod->spacing);
            EXPECT_EQ(node.key.y, y * lod->spacing);
            EXPECT_EQ(
                static_cast<uint8_t>(canonical_fields[source_index][2U]),
                static_cast<uint8_t>(node.sample.top_material)
            ) << "V21 canonical and world L0 materials differ at source index " << source_index;
        }
    }
    auto const expectCoastalCellMaterial = [
        &canonical_fields, &lod
    ](uint32_t const cell_x, uint32_t const cell_y, shared::Block const expected_material) {
        size_t const index = static_cast<size_t>(cell_y) * SOURCE_RESOLUTION + cell_x;
        EXPECT_EQ(static_cast<uint8_t>(canonical_fields[index][2U]), static_cast<uint8_t>(expected_material));
        uint32_t const world_x = cell_x * lod->spacing;
        uint32_t const world_y = cell_y * lod->spacing;
        auto const node = std::find_if(lod->nodes.begin(), lod->nodes.end(), [world_x, world_y](auto const& candidate) {
            return candidate.key.x == world_x && candidate.key.y == world_y;
        });
        ASSERT_NE(node, lod->nodes.end());
        EXPECT_EQ(node->sample.top_material, expected_material);
    };
    expectCoastalCellMaterial(242U, 0U, shared::Block::Snow);
    expectCoastalCellMaterial(203U, 254U, shared::Block::Snow);
    for (size_t const index : {size_t{0U}, size_t{37U * SOURCE_RESOLUTION + 91U}, SOURCE_COUNT - 1U})
    {
        double const world_x_m = static_cast<double>(index % SOURCE_RESOLUTION) * 256.0;
        double const world_y_m = static_cast<double>(index / SOURCE_RESOLUTION) * 256.0;
        double const formed_l0_elevation_m = generation::v21L0ElevationAt(*atlas, world_x_m, world_y_m);
        EXPECT_DOUBLE_EQ(image->v21_elevation_m[index], formed_l0_elevation_m);
        EXPECT_DOUBLE_EQ(value_at(*elevation, index), formed_l0_elevation_m);
        EXPECT_FLOAT_EQ(canonical_fields[index][1U], static_cast<float>(formed_l0_elevation_m));
        EXPECT_TRUE(shared::isValidBlock(static_cast<shared::Block>(
            static_cast<uint8_t>(canonical_fields[index][2U])
        )));
    }

    auto const stock_map = client::rasterizeGenerationMap(*atlas);
    std::vector<uint8_t> const ignored_proxy_ice(SOURCE_COUNT, client::GENERATION_MAP_ICE_NONE);
    auto const proxy_map = client::rasterizeGenerationMap(*atlas, ignored_proxy_ice);
    ASSERT_TRUE(stock_map);
    ASSERT_TRUE(proxy_map);
    EXPECT_EQ(stock_map->rgba, proxy_map->rgba);
    client::GenerationMapAnimator animator;
    auto const animated = animator.update(nullptr, atlas, nullptr, nullptr,
        {.visible_elapsed = std::chrono::milliseconds{1'000}});
    ASSERT_NE(animated.image, nullptr);
    EXPECT_EQ(animated.image->width, client::GENERATION_MAP_ANIMATION_RENDER_RESOLUTION);
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
    auto const shared_base = client::rasterizeGenerationMap(*world->atlas(), ice_mask->surface_kind, false);
    auto const generation_palette = client::rasterizeGenerationMap(*world->atlas(), ice_mask->surface_kind);
    ASSERT_TRUE(shared_base.has_value());
    ASSERT_TRUE(generation_palette.has_value());
    EXPECT_NE(generation_palette->rgba, shared_base->rgba);

    client::S2WorldMapState state;
    state.layers.fill(false);
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->rgba.front(), shared_base->rgba.front());
}

TEST(S2MapPresentationTest, PlateBoundariesStayVectorDataWhenTheLayerIsEnabled)
{
    auto const source_world = mapWorld();
    auto atlas = std::make_shared<generation::GlobalAtlas>(*source_world->atlas());
    for (uint32_t y = 0U; y < atlas->configuration.resolution; ++y) {
        for (uint32_t x = 0U; x < atlas->configuration.resolution; ++x) {
            atlas->plate_id[static_cast<size_t>(y) * atlas->configuration.resolution + x]
                = x < atlas->configuration.resolution / 2U ? 1 : 2;
        }
    }
    auto const world = std::make_shared<shared::TerrainWorld const>(std::move(atlas));
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    auto const base = client::rasterizeS2WorldMap(*world, *lod, state);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto plates = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(base);
    ASSERT_TRUE(plates);
    EXPECT_EQ(plates->rgba, base->rgba);
    ASSERT_FALSE(plates->plate_boundaries.empty());
    EXPECT_TRUE(std::any_of(plates->plate_boundaries.begin(), plates->plate_boundaries.end(), [](auto const& edge) {
        return edge.start[0U] == 0.5F && edge.end[0U] == 0.5F
            && edge.start[1U] == 0.0F && edge.end[1U] == 1.0F;
    }));
    EXPECT_TRUE(std::any_of(plates->plate_boundaries.begin(), plates->plate_boundaries.end(), [](auto const& edge) {
        return edge.start[0U] == 1.0F && edge.end[0U] == 1.0F
            && edge.start[1U] == 0.0F && edge.end[1U] == 1.0F;
    }));

    plates->plate_motions.clear();
    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(
        state, &*plates, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, triangles));
    std::array<float, 2U> plate_line_vertical_extent{
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
    };
    size_t opaque_plate_vertices = 0U;
    for (client::GuiVectorVertex const& vertex : triangles) {
        if (vertex.rgba != (std::array<uint8_t, 4U>{213U, 164U, 237U, 255U})) {
            continue;
        }
        ++opaque_plate_vertices;
        plate_line_vertical_extent[0U] = std::min(plate_line_vertical_extent[0U], vertex.position[1U]);
        plate_line_vertical_extent[1U] = std::max(plate_line_vertical_extent[1U], vertex.position[1U]);
    }
    EXPECT_GE(opaque_plate_vertices, 12U);
    EXPECT_GT(plate_line_vertical_extent[1U] - plate_line_vertical_extent[0U], 300.0F);

    std::array<bool, 2U> seam_edges{};
    for (client::GuiVectorVertex const& vertex : triangles) {
        if (vertex.rgba != (std::array<uint8_t, 4U>{213U, 164U, 237U, 255U})) {
            continue;
        }
        seam_edges[0U] = seam_edges[0U] || vertex.position[0U] < 8.0F;
        seam_edges[1U] = seam_edges[1U] || vertex.position[0U] > 1'272.0F;
    }
    EXPECT_TRUE(seam_edges[0U]);
    EXPECT_TRUE(seam_edges[1U]);

    client::S2WorldMapState wrapped_state = state;
    wrapped_state.center_x = 65'530.0;
    wrapped_state.zoom = 2.0;
    triangles.clear();
    static_cast<void>(client::buildS2WorldMapOverlay(
        wrapped_state, &*plates, world.get(), wrapped_state.center_x, wrapped_state.center_y,
        1'280U, 676U, 1.0F, labels, triangles));
    float closest_wrapped_seam_vertex = std::numeric_limits<float>::infinity();
    for (client::GuiVectorVertex const& vertex : triangles) {
        if (vertex.rgba == (std::array<uint8_t, 4U>{213U, 164U, 237U, 255U})) {
            closest_wrapped_seam_vertex = std::min(
                closest_wrapped_seam_vertex, std::abs(vertex.position[0U] - 640.0F));
        }
    }
    EXPECT_LT(closest_wrapped_seam_vertex, 1.5F);
}

TEST(S2MapPresentationTest, StaircasePlateContoursKeepTheirJoinsInsteadOfDroppingShortRuns)
{
    auto const source_world = mapWorld();
    auto atlas = std::make_shared<generation::GlobalAtlas>(*source_world->atlas());
    uint32_t const resolution = atlas->configuration.resolution;
    for (uint32_t y = 0U; y < resolution; ++y) {
        for (uint32_t x = 0U; x < resolution; ++x) {
            atlas->plate_id[static_cast<size_t>(y) * resolution + x] = x < y ? 1 : 2;
        }
    }
    auto const world = std::make_shared<shared::TerrainWorld const>(std::move(atlas));
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    ASSERT_FALSE(image->plate_boundaries.empty());
    // Preserve the ownership contour, including one-cell corridors and junctions.
    // Diagonal shortcuts are not allowed to cut across a neighboring plate.
    EXPECT_GE(image->plate_boundaries.size(), 256U);
    for (auto const& edge : image->plate_boundaries) {
        EXPECT_TRUE(edge.start[0U] == edge.end[0U] || edge.start[1U] == edge.end[1U]);
    }
    for (auto const& edge : image->plate_boundaries) {
        for (auto const endpoint : {edge.start, edge.end}) {
            if (endpoint[0U] <= 0.0F || endpoint[0U] >= 1.0F
                || endpoint[1U] <= 0.0F || endpoint[1U] >= 1.0F) {
                continue;
            }
            auto const joins = std::count_if(image->plate_boundaries.begin(), image->plate_boundaries.end(),
                [endpoint](auto const& other) { return other.start == endpoint || other.end == endpoint; });
            EXPECT_GE(joins, 2);
        }
    }
    auto contour_image = *image;
    contour_image.plate_motions.clear();
    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(
        state, &contour_image, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, triangles));
    EXPECT_FALSE(triangles.empty());

}

TEST(S2MapPresentationTest, RainLayerLeavesTheTerrainRasterUntinted)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);

    auto const terrain = client::rasterizeS2WorldMap(*world, *lod, state);
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Precipitation)] = true;
    auto const rain = client::rasterizeS2WorldMap(*world, *lod, state);

    ASSERT_TRUE(terrain);
    ASSERT_TRUE(rain);
    EXPECT_EQ(rain->rgba, terrain->rgba);
}

TEST(S2MapPresentationTest, RainParticlesAnimateAndUseLocalPrecipitationDensity)
{
    auto const source_world = mapWorld();
    auto atlas = std::make_shared<generation::GlobalAtlas>(*source_world->atlas());
    for (uint32_t y = 0U; y < atlas->configuration.resolution; ++y) {
        for (uint32_t x = 0U; x < atlas->configuration.resolution; ++x) {
            atlas->precipitation[static_cast<size_t>(y) * atlas->configuration.resolution + x]
                = x < atlas->configuration.resolution / 2U ? 0.0F : 1'000.0F;
        }
    }
    auto const world = std::make_shared<shared::TerrainWorld const>(std::move(atlas));
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Precipitation)] = true;
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    client::S2WorldMapState terrain_state = state;
    terrain_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Precipitation)] = false;
    auto const terrain = client::rasterizeS2WorldMap(*world, *lod, terrain_state);
    ASSERT_TRUE(terrain);
    EXPECT_EQ(image->rgba, terrain->rgba);

    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> first_frame;
    std::vector<client::GuiVectorVertex> later_frame;
    static_cast<void>(client::buildS2WorldMapOverlay(
        state, &*image, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, first_frame, 0.0F));
    static_cast<void>(client::buildS2WorldMapOverlay(
        state, &*image, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, later_frame, 0.5F));

    std::array<uint8_t, 4U> const rain_color{210U, 230U, 235U, 255U};
    auto const rain_vertices = [rain_color](std::vector<client::GuiVectorVertex> const& vertices) {
        return std::count_if(vertices.begin(), vertices.end(), [rain_color](auto const& vertex) {
            return vertex.rgba == rain_color;
        });
    };
    int64_t left_rain = 0;
    int64_t right_rain = 0;
    for (auto const& vertex : first_frame) {
        if (vertex.rgba != rain_color) {
            continue;
        }
        (vertex.position[0U] < 640.0F ? left_rain : right_rain)++;
    }
    EXPECT_GT(right_rain, left_rain);
    EXPECT_GT(rain_vertices(first_frame), 0);
    EXPECT_GT(rain_vertices(later_frame), 0);
    size_t moved_vertices = 0U;
    for (size_t index = 0U; index < std::min(first_frame.size(), later_frame.size()); ++index) {
        moved_vertices += first_frame[index].position != later_frame[index].position ? 1U : 0U;
    }
    EXPECT_GT(moved_vertices, 0U);
}

TEST(S2MapPresentationTest, MinimapMarkerStaysCenteredOutsideLocalCoverageAndAcrossWrap)
{
    static constexpr std::array<float, 4U> BOUNDS{10.0F, 20.0F, 100.0F, 100.0F};
    static constexpr float CENTER_X = BOUNDS[0U] + BOUNDS[2U] * 0.5F;
    static constexpr float CENTER_Y = BOUNDS[1U] + BOUNDS[3U] * 0.5F;
    auto lod = localLod();
    auto const north_up = client::rasterizeS2Minimap(lod);
    auto const west_side_player = client::layoutS2MinimapMarker(lod, 500.0, 500.0, 0.0, BOUNDS);
    auto const outside_loaded_window = client::layoutS2MinimapMarker(lod, 32'768.0, 32'768.0, 90.0, BOUNDS);
    auto const wrapped_player = client::layoutS2MinimapMarker(lod, 65'530.0, 65'530.0, 180.0, BOUNDS);

    ASSERT_TRUE(north_up.has_value());
    EXPECT_EQ(north_up->width, client::S2_MINIMAP_IMAGE_SIDE);
    EXPECT_EQ(north_up->source_identity, lod.terrain_identity);
    for (client::S2MinimapMarker const& marker : {west_side_player, outside_loaded_window, wrapped_player}) {
        EXPECT_TRUE(marker.visible);
        EXPECT_FLOAT_EQ(marker.x, CENTER_X);
        EXPECT_FLOAT_EQ(marker.y, CENTER_Y);
    }
    EXPECT_DOUBLE_EQ(west_side_player.heading_degrees, 0.0);
    EXPECT_DOUBLE_EQ(outside_loaded_window.heading_degrees, 90.0);
    EXPECT_DOUBLE_EQ(wrapped_player.heading_degrees, 180.0);
}

TEST(S2MapPresentationTest, MinimapTerrainAndMarkerShareNorthUpWorldY)
{
    auto lod = localLod();
    for (shared::TerrainLodNode& node : lod.nodes) {
        node.sample.height = 400.0;
        node.sample.top_material = node.key.y >= lod.extent / 2U
            ? shared::Block::Sand
            : shared::Block::Stone;
        node.sample.water_present = false;
    }

    auto const image = client::rasterizeS2Minimap(lod);
    auto const south = client::layoutS2MinimapMarker(lod, 8192.0, 4096.0, 0.0, {0.0F, 0.0F, 100.0F, 100.0F});
    auto const north = client::layoutS2MinimapMarker(lod, 8192.0, 12'288.0, 0.0, {0.0F, 0.0F, 100.0F, 100.0F});

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->rgba[image->width + 1U], (std::array<uint8_t, 4U>{211U, 194U, 149U, 255U}));
    EXPECT_EQ(image->rgba[(image->height - 2U) * image->width + 1U],
        (std::array<uint8_t, 4U>{118U, 118U, 118U, 255U}));
    ASSERT_TRUE(south.visible);
    ASSERT_TRUE(north.visible);
    EXPECT_FLOAT_EQ(south.x, 50.0F);
    EXPECT_FLOAT_EQ(south.y, 50.0F);
    EXPECT_FLOAT_EQ(north.x, 50.0F);
    EXPECT_FLOAT_EQ(north.y, 50.0F);

    std::vector<client::GuiVectorVertex> pointer;
    client::appendS2MinimapPointerTriangles(north, {0.0F, 0.0F, 100.0F, 100.0F}, 1.0F, pointer);
    ASSERT_FALSE(pointer.empty());
    float pointer_tip_y = north.y;
    for (client::GuiVectorVertex const& vertex : pointer) {
        pointer_tip_y = std::min(pointer_tip_y, vertex.position[1U]);
    }
    EXPECT_LT(pointer_tip_y, north.y);
}

TEST(S2MapPresentationTest, MinimapHeadingFollowsCameraTurnsAndCompassCardinals)
{
    static constexpr std::array<double, 4U> CARDINAL_YAWS{0.0, 90.0, 180.0, 270.0};
    client::Camera camera;
    for (double const yaw : CARDINAL_YAWS) {
        ASSERT_TRUE(camera.setAngles({.yaw_degrees = yaw}));
        glm::dvec3 const facing = camera.forward();
        EXPECT_NEAR(client::minimapHeadingDegreesFromCameraForward(facing.x, facing.y), yaw, 1e-12);
    }

    ASSERT_TRUE(camera.setAngles({}));
    ASSERT_TRUE(camera.rotate(-15.0, 0.0));
    glm::dvec3 const left_facing = camera.forward();
    EXPECT_NEAR(client::minimapHeadingDegreesFromCameraForward(left_facing.x, left_facing.y), 345.0, 1e-12);

    ASSERT_TRUE(camera.setAngles({}));
    ASSERT_TRUE(camera.rotate(15.0, 0.0));
    glm::dvec3 const right_facing = camera.forward();
    EXPECT_NEAR(client::minimapHeadingDegreesFromCameraForward(right_facing.x, right_facing.y), 15.0, 1e-12);
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
        1'280U, 676U, 1.0F, labels, triangles, 0.0F);

    ASSERT_FALSE(triangles.empty());
    ASSERT_EQ(triangles.size() % 3U, 0U);
    EXPECT_LE(triangles.size(), client::MAXIMUM_GUI_VECTOR_VERTICES - 1U);
    std::vector<client::GuiVectorVertex> later_frame;
    std::vector<client::GuiVectorVertex> repeated_frame;
    static_cast<void>(client::buildS2WorldMapOverlay(
        raster_state, &*vector_layers, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, repeated_frame, 0.0F));
    static_cast<void>(client::buildS2WorldMapOverlay(
        raster_state, &*vector_layers, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, later_frame, 0.5F));
    ASSERT_FALSE(later_frame.empty());
    ASSERT_EQ(repeated_frame.size(), triangles.size());
    for (size_t index = 0U; index < triangles.size(); ++index) {
        EXPECT_EQ(repeated_frame[index].position, triangles[index].position);
        EXPECT_EQ(repeated_frame[index].rgba, triangles[index].rgba);
    }
    size_t moving_vertices = 0U;
    for (size_t index = 0U; index < std::min(triangles.size(), later_frame.size()); ++index) {
        if (triangles[index].position != later_frame[index].position) {
            ++moving_vertices;
        }
    }
    EXPECT_GT(moving_vertices, 0U);
    std::array<size_t, 4U> wind_vertices_by_quadrant{};
    for (client::GuiVectorVertex const& vertex : triangles) {
        if (vertex.rgba != (std::array<uint8_t, 4U>{62U, 232U, 216U, 255U})) {
            continue;
        }
        size_t const quadrant = (vertex.position[0U] >= 640.0F ? 1U : 0U)
            + (vertex.position[1U] >= 338.0F ? 2U : 0U);
        ++wind_vertices_by_quadrant[quadrant];
    }
    for (size_t const count : wind_vertices_by_quadrant) {
        EXPECT_GT(count, 0U);
    }
    EXPECT_EQ(vector_layers->rgba, base->rgba);
    EXPECT_TRUE(std::any_of(triangles.begin(), triangles.end(), [](auto const& vertex) {
        return vertex.rgba == (std::array<uint8_t, 4U>{62U, 232U, 216U, 255U});
    }));
    EXPECT_TRUE(std::any_of(triangles.begin(), triangles.end(), [](auto const& vertex) {
        return vertex.rgba == (std::array<uint8_t, 4U>{162U, 224U, 249U, 255U});
    }));
    client::S2WorldMapState wide_state = raster_state;
    wide_state.zoom = client::minimumS2WorldMapZoom(1'800U, 600U);
    std::vector<client::GuiVectorVertex> wide_triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(
        wide_state, &*vector_layers, world.get(), 32'768.0, 32'768.0,
        1'800U, 600U, 1.0F, labels, wide_triangles, 0.0F));
    std::array<size_t, 3U> const wind_vertices_by_world_copy = [&] {
        std::array<size_t, 3U> counts{};
        for (client::GuiVectorVertex const& vertex : wide_triangles) {
            if (vertex.rgba == (std::array<uint8_t, 4U>{62U, 232U, 216U, 255U})
                && vertex.position[0U] >= 0.0F && vertex.position[0U] < 1'800.0F) {
                size_t const copy = std::min(static_cast<size_t>(vertex.position[0U] / 600.0F), size_t{2U});
                ++counts[copy];
            }
        }
        return counts;
    }();
    for (size_t const count : wind_vertices_by_world_copy) {
        EXPECT_GT(count, 0U);
    }
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

TEST(S2MapPresentationTest, CurrentArrowsAndParticlesStayOnOceanDuringAdvection)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t COAST_X = RESOLUTION / 2U;
    auto const source_world = mapWorld();
    auto atlas = std::make_shared<generation::GlobalAtlas>(*source_world->atlas());
    for (uint32_t y = 0U; y < RESOLUTION; ++y) {
        for (uint32_t x = 0U; x < RESOLUTION; ++x) {
            size_t const index = static_cast<size_t>(y) * RESOLUTION + x;
            atlas->ocean_mask[index] = x >= COAST_X ? 1U : 0U;
            atlas->current_u[index] = -1.0F;
            atlas->current_v[index] = 0.0F;
        }
    }
    auto const world = std::make_shared<shared::TerrainWorld const>(std::move(atlas));
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)] = true;
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);

    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(
        state, &*image, world.get(), 32'768.0, 32'768.0,
        1'280U, 676U, 1.0F, labels, triangles, 0.37F));

    std::array<size_t, 2U> current_vertices{};
    for (client::GuiVectorVertex const& vertex : triangles) {
        bool const current_arrow = vertex.rgba[0U] == 162U && vertex.rgba[1U] == 224U
            && vertex.rgba[2U] == 249U;
        bool const current_foam = vertex.rgba[0U] == 140U && vertex.rgba[1U] == 195U
            && vertex.rgba[2U] == 211U;
        if (!current_arrow && !current_foam) {
            continue;
        }
        ++current_vertices[vertex.position[0U] < 640.0F ? 0U : 1U];
    }
    EXPECT_EQ(current_vertices[0U], 0U);
    EXPECT_GT(current_vertices[1U], 0U);
}

TEST(S2MapPresentationTest, PlateMotionAndNamesUseTheScreenOverlayBatch)
{
    static constexpr std::array<uint8_t, 4U> PLATE_MOTION_COLOR{255U, 184U, 64U, 255U};
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
    auto const plate_label = std::find_if(labels.labels().begin(), labels.labels().end(),
        [](client::TextLabel const& label) {
            return label.value == "P1"
                && label.color.red == static_cast<float>(PLATE_MOTION_COLOR[0U]) / 255.0F
                && label.color.green == static_cast<float>(PLATE_MOTION_COLOR[1U]) / 255.0F
                && label.color.blue == static_cast<float>(PLATE_MOTION_COLOR[2U]) / 255.0F;
        });
    ASSERT_NE(plate_label, labels.labels().end());
    EXPECT_FLOAT_EQ(plate_label->color.red, static_cast<float>(PLATE_MOTION_COLOR[0U]) / 255.0F);
    EXPECT_FLOAT_EQ(plate_label->color.green, static_cast<float>(PLATE_MOTION_COLOR[1U]) / 255.0F);
    EXPECT_FLOAT_EQ(plate_label->color.blue, static_cast<float>(PLATE_MOTION_COLOR[2U]) / 255.0F);
    std::array<float, 4U> arrow_bounds{
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
    };
    for (client::GuiVectorVertex const& vertex : triangles) {
        if (vertex.rgba != PLATE_MOTION_COLOR) {
            continue;
        }
        arrow_bounds[0U] = std::min(arrow_bounds[0U], vertex.position[0U]);
        arrow_bounds[1U] = std::min(arrow_bounds[1U], vertex.position[1U]);
        arrow_bounds[2U] = std::max(arrow_bounds[2U], vertex.position[0U]);
        arrow_bounds[3U] = std::max(arrow_bounds[3U], vertex.position[1U]);
    }
    ASSERT_TRUE(std::isfinite(arrow_bounds[0U]));
    EXPECT_GT(std::max(arrow_bounds[2U] - arrow_bounds[0U], arrow_bounds[3U] - arrow_bounds[1U]), 40.5F);
}

TEST(S2MapPresentationTest, MinimapLodCentersOnCurrentPlayerWorldCoordinates)
{
    auto const world = mapWorld();
    for (auto const [player_x, player_y] : {
             std::pair{500.0, 500.0},
             std::pair{65'530.0, 65'530.0},
         }) {
        auto const lod = client::buildS2MinimapLod(*world, player_x, player_y);
        ASSERT_NE(lod, nullptr);
        uint32_t const center_index = lod->side / 2U;
        shared::TerrainLodNode const& center = lod->nodes[
            static_cast<size_t>(center_index) * lod->side + center_index];
        auto const wrapped_distance = [](uint32_t const a, uint32_t const b) {
            uint32_t const direct = a > b ? a - b : b - a;
            return std::min(direct, generation::WORLD_SIZE_BLOCKS - direct);
        };
        EXPECT_LE(wrapped_distance(center.key.x, static_cast<uint32_t>(player_x)), lod->spacing);
        EXPECT_LE(wrapped_distance(center.key.y, static_cast<uint32_t>(player_y)), lod->spacing);
        auto const marker = client::layoutS2MinimapMarker(*lod, player_x, player_y, 45.0,
            {10.0F, 20.0F, 100.0F, 100.0F});
        EXPECT_TRUE(marker.visible);
        EXPECT_FLOAT_EQ(marker.x, 60.0F);
        EXPECT_FLOAT_EQ(marker.y, 70.0F);
    }
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

TEST(S2MapPresentationTest, MapLayoutWrapsCloseAndWholeWorldViewsAcrossTheFullViewport)
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
    state.center_x = 32'768.0;
    state.center_y = 32'768.0;
    client::S2MapLayout const overview = client::layoutS2Map(state, 1'200U, 800U, true, true);
    EXPECT_GT(overview.world_draw_count, 1U);
    EXPECT_LE(overview.world_draw_count, overview.world_draws.size());
    EXPECT_FALSE(overview.show_minimap);
    float destination_area = 0.0F;
    float source_area = 0.0F;
    for (uint32_t index = 0U; index < overview.world_draw_count; ++index) {
        auto const& draw = overview.world_draws[index];
        ASSERT_TRUE(client::validMapImageDraw(draw));
        destination_area += draw.destination[2U] * draw.destination[3U];
        source_area += draw.source[2U] * draw.source[3U];
        EXPECT_NEAR(draw.destination[2U] / draw.source[2U], 800.0F, 0.001F);
        EXPECT_NEAR(draw.destination[3U] / draw.source[3U], 800.0F, 0.001F);
    }
    EXPECT_NEAR(destination_area, 1'200.0F * 800.0F, 1.0F);
    EXPECT_NEAR(source_area, 1.5F, 0.001F);

    client::S2MapLayout const minimap = client::layoutS2Map({}, 1'280U, 676U, false, true);
    ASSERT_TRUE(minimap.show_minimap);
    EXPECT_EQ(minimap.minimap_bounds, (std::array<float, 4U>{1'054.0F, 44.0F, 192.0F, 192.0F}));

    client::S2MapLayout const hidpi_minimap = client::layoutS2Map(
        {}, 2'560U, 1'352U, false, true, 2.0F);
    ASSERT_TRUE(hidpi_minimap.show_minimap);
    EXPECT_EQ(hidpi_minimap.minimap_bounds,
        (std::array<float, 4U>{2'108.0F, 88.0F, 384.0F, 384.0F}));
}

TEST(S2MapPresentationTest, ZoomOutFloorFitsOneWorldPeriodAndPreservesWrappedPan)
{
    EXPECT_FLOAT_EQ(client::minimumS2WorldMapZoom(1'200U, 800U), 2.0F / 3.0F);
    EXPECT_FLOAT_EQ(client::minimumS2WorldMapZoom(800U, 1'200U), 2.0F / 3.0F);
    EXPECT_FLOAT_EQ(client::minimumS2WorldMapZoom(900U, 900U), 1.0F);
    EXPECT_FLOAT_EQ(client::minimumS2WorldMapZoom(0U, 900U), 1.0F);

    client::S2WorldMapState state{
        .center_x = 100.0,
        .center_y = static_cast<double>(generation::WORLD_SIZE_BLOCKS) * 0.75,
        .zoom = 1.0,
    };
    for (uint32_t index = 0U; index < 12U; ++index) {
        EXPECT_TRUE(client::applyS2WorldMapAction(
            state, client::S2WorldMapAction::ZoomOut, 2'000.0, 4'000.0, false, 1'200U, 800U));
    }
    EXPECT_DOUBLE_EQ(state.zoom, client::minimumS2WorldMapZoom(1'200U, 800U));

    client::S2MapLayout const layout = client::layoutS2Map(state, 1'200U, 800U, true, false);
    ASSERT_GT(layout.world_draw_count, 0U);
    ASSERT_LE(layout.world_draw_count, layout.world_draws.size());
    float destination_area = 0.0F;
    float source_area = 0.0F;
    for (uint32_t index = 0U; index < layout.world_draw_count; ++index) {
        auto const& draw = layout.world_draws[index];
        ASSERT_TRUE(client::validMapImageDraw(draw));
        destination_area += draw.destination[2U] * draw.destination[3U];
        source_area += draw.source[2U] * draw.source[3U];
        EXPECT_NEAR(draw.destination[2U] / draw.source[2U], 800.0F, 0.001F);
        EXPECT_NEAR(draw.destination[3U] / draw.source[3U], 800.0F, 0.001F);
    }
    EXPECT_NEAR(destination_area, 1'200.0F * 800.0F, 1.0F);
    EXPECT_NEAR(source_area, 1.5F, 0.001F);

    double const previous_center = state.center_x;
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::PanLeft, 2'000.0, 4'000.0, true, 1'200U, 800U));
    EXPECT_NE(state.center_x, previous_center);
    EXPECT_GT(state.center_x, static_cast<double>(generation::WORLD_SIZE_BLOCKS) * 0.95);
    EXPECT_GE(state.center_x, 0.0);
    EXPECT_LT(state.center_x, generation::WORLD_SIZE_BLOCKS);

    for (auto const [width, height] : {std::pair{844U, 346U}, std::pair{346U, 844U}}) {
        client::S2WorldMapState aspect_state;
        for (uint32_t index = 0U; index < 12U; ++index) {
            static_cast<void>(client::applyS2WorldMapAction(
                aspect_state, client::S2WorldMapAction::ZoomOut, 0.0, 0.0, false, width, height));
        }
        EXPECT_DOUBLE_EQ(aspect_state.zoom, client::minimumS2WorldMapZoom(width, height));
        auto const aspect_layout = client::layoutS2Map(aspect_state, width, height, true, false);
        ASSERT_GT(aspect_layout.world_draw_count, 0U);
        ASSERT_LE(aspect_layout.world_draw_count, aspect_layout.world_draws.size());
        float visible_area = 0.0F;
        float source_area = 0.0F;
        for (uint32_t index = 0U; index < aspect_layout.world_draw_count; ++index) {
            auto const& draw = aspect_layout.world_draws[index];
            ASSERT_TRUE(client::validMapImageDraw(draw));
            visible_area += draw.destination[2U] * draw.destination[3U];
            source_area += draw.source[2U] * draw.source[3U];
            EXPECT_NEAR(draw.destination[2U] / draw.source[2U], static_cast<float>(std::min(width, height)), 0.001F);
            EXPECT_NEAR(draw.destination[3U] / draw.source[3U], static_cast<float>(std::min(width, height)), 0.001F);
        }
        EXPECT_NEAR(visible_area, static_cast<float>(width) * height, 1.0F);
        EXPECT_NEAR(source_area, static_cast<float>(std::max(width, height)) / std::min(width, height), 0.001F);
    }

    client::S2WorldMapState ultrawide_state{
        .center_x = static_cast<double>(generation::WORLD_SIZE_BLOCKS) * 0.5,
        .center_y = static_cast<double>(generation::WORLD_SIZE_BLOCKS) * 0.25,
    };
    for (uint32_t index = 0U; index < 12U; ++index) {
        static_cast<void>(client::applyS2WorldMapAction(
            ultrawide_state, client::S2WorldMapAction::ZoomOut, 0.0, 0.0, false, 10'000U, 1'000U));
    }
    auto const ultrawide_layout = client::layoutS2Map(ultrawide_state, 10'000U, 1'000U, true, false);
    EXPECT_EQ(ultrawide_layout.world_draw_count, 22U);
    ASSERT_GT(ultrawide_layout.world_draw_count, 16U);
    ASSERT_LE(ultrawide_layout.world_draw_count, ultrawide_layout.world_draws.size());
    float ultrawide_destination_area = 0.0F;
    float ultrawide_source_area = 0.0F;
    for (uint32_t index = 0U; index < ultrawide_layout.world_draw_count; ++index) {
        auto const& draw = ultrawide_layout.world_draws[index];
        ASSERT_TRUE(client::validMapImageDraw(draw));
        ultrawide_destination_area += draw.destination[2U] * draw.destination[3U];
        ultrawide_source_area += draw.source[2U] * draw.source[3U];
        EXPECT_NEAR(draw.destination[2U] / draw.source[2U], 1'000.0F, 0.001F);
        EXPECT_NEAR(draw.destination[3U] / draw.source[3U], 1'000.0F, 0.001F);
    }
    EXPECT_NEAR(ultrawide_destination_area, 10'000'000.0F, 1.0F);
    EXPECT_NEAR(ultrawide_source_area, 10.0F, 0.001F);
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

TEST(S2MapPresentationTest, WorldMapViewRejectsCancellationBeforeComposingValidMap)
{
    client::S2MapImage source;
    source.width = 2U;
    source.height = 2U;
    source.rgba.resize(4U, {20U, 40U, 60U, 255U});
    auto const layout = client::layoutS2Map({}, 4U, 2U, true, false);

    ASSERT_TRUE(client::composeS2WorldMapView(&source, layout, {}, 4U, 2U));
    std::stop_source cancellation;
    cancellation.request_stop();

    EXPECT_THROW(
        static_cast<void>(client::composeS2WorldMapView(&source, layout, {}, 4U, 2U, cancellation.get_token())),
        generation::GenerationCancelled
    );
}

TEST(S2MapPresentationTest, WorldMapUsesBakedRiversUntilViewportLayerIsReady)
{
    client::S2MapImage source;
    source.width = 1U;
    source.height = 1U;
    source.rgba = {{20U, 60U, 130U, 255U}};
    source.v10_river_base_rgba = {{90U, 100U, 70U, 255U}};
    client::S2MapLayout const layout = client::layoutS2Map({}, 1U, 1U, true, false);

    auto const fallback = client::composeS2WorldMapViewWithRiverLayer(&source, layout, {}, 1U, 1U, nullptr);
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback->rgba, source.rgba);

    client::S2MapImage layer;
    layer.width = 1U;
    layer.height = 1U;
    layer.rgba = {{0U, 0U, 0U, 0U}};
    auto const refined = client::composeS2WorldMapViewWithRiverLayer(&source, layout, {}, 1U, 1U, &layer);
    ASSERT_TRUE(refined);
    EXPECT_EQ(refined->rgba, source.v10_river_base_rgba);
}

TEST(S2MapPresentationTest, LayerAndNavigationActionsAreIndependentAndBounded)
{
    client::S2WorldMapState state;
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::ToggleWind, 2'000.0, 4'000.0, false, 1'200U, 800U));
    EXPECT_TRUE(state.layers[static_cast<size_t>(client::S2WorldMapLayer::Wind)]);
    EXPECT_FALSE(state.layers[static_cast<size_t>(client::S2WorldMapLayer::Currents)]);

    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::PanLeft, 2'000.0, 4'000.0, true, 1'200U, 800U));
    EXPECT_GE(state.center_x, 0.0);
    EXPECT_LT(state.center_x, generation::WORLD_SIZE_BLOCKS);
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::Overview, 2'000.0, 4'000.0, false, 1'200U, 800U));
    EXPECT_TRUE(state.overview);
    EXPECT_TRUE(client::applyS2WorldMapAction(
        state, client::S2WorldMapAction::ZoomIn, 2'000.0, 4'000.0, false, 1'200U, 800U));
    EXPECT_FALSE(state.overview);
    EXPECT_GT(state.zoom, client::minimumS2WorldMapZoom(1'200U, 800U));
}

TEST(S2MapPresentationTest, WeatherTogglesReuseThePreparedRaster)
{
    using Action = client::S2WorldMapAction;
    client::S2WorldMapState state;
    auto const prepared_layers = state.rasterLayers();
    for (Action const action : {Action::ToggleCurrents, Action::ToggleWind, Action::TogglePrecipitation}) {
        ASSERT_TRUE(client::applyS2WorldMapAction(state, action, 0.0, 0.0, false, 1'920U, 1'152U));
        EXPECT_EQ(state.rasterLayers(), prepared_layers);
    }
    for (Action const action : {
        Action::TogglePlates, Action::ToggleHeights, Action::ToggleTemperature,
        Action::ToggleBiomes, Action::ToggleMoisture,
    }) {
        ASSERT_TRUE(client::applyS2WorldMapAction(state, action, 0.0, 0.0, false, 1'920U, 1'152U));
        EXPECT_NE(state.rasterLayers(), prepared_layers);
        ASSERT_TRUE(client::applyS2WorldMapAction(state, action, 0.0, 0.0, false, 1'920U, 1'152U));
        EXPECT_EQ(state.rasterLayers(), prepared_layers);
    }
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

TEST(S2MapPresentationTest, FlowArrowRemainsRigidWhenTangentAndPositionChange)
{
    auto const first = client::s2FlowArrowSegments({100.0F, 100.0F}, {1.0F, 0.0F}, 16.0F);
    auto const rotated = client::s2FlowArrowSegments({120.0F, 140.0F}, {0.0F, 3.0F}, 16.0F);
    for (size_t segment = 0U; segment < 3U; ++segment) {
        auto const length = [segment](auto const& points) {
            auto const& a = points[segment * 2U];
            auto const& b = points[segment * 2U + 1U];
            return std::hypot(b[0U] - a[0U], b[1U] - a[1U]);
        };
        EXPECT_FLOAT_EQ(length(first), length(rotated));
    }
    EXPECT_FLOAT_EQ(first[0U][1U], first[1U][1U]);
    EXPECT_FLOAT_EQ(rotated[0U][0U], rotated[1U][0U]);
    EXPECT_GT(rotated[1U][1U], rotated[0U][1U]);
    EXPECT_EQ(first[1U], first[2U]);
    EXPECT_EQ(first[1U], first[4U]);
}

TEST(S2MapPresentationTest, V10PlateMotionAnchorsBelongToTheirActualNativePlate)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr);
    shared::TerrainWorld world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    client::S2WorldMapState state;
    state.layers.fill(false);
    auto const image = client::rasterizeS2WorldMap(world, *lod, state);
    ASSERT_TRUE(image);
    auto const plate_ids = generation::v10CurrentPlateIds(*atlas);
    auto const directions = generation::v10PlateDirections(*atlas);
    ASSERT_EQ(plate_ids.size(), 256U * 256U);
    ASSERT_EQ(image->v10_plate_ids, (std::vector<uint32_t>{plate_ids.begin(), plate_ids.end()}));
    ASSERT_FALSE(image->plate_motions.empty());
    for (auto const& motion : image->plate_motions) {
        SCOPED_TRACE(motion.id);
        ASSERT_GE(motion.x, 0.0F);
        ASSERT_LT(motion.x, 1.0F);
        ASSERT_GE(motion.y, 0.0F);
        ASSERT_LT(motion.y, 1.0F);
        uint32_t const x = static_cast<uint32_t>(motion.x * 256.0F);
        uint32_t const y = static_cast<uint32_t>(motion.y * 256.0F);
        EXPECT_EQ(plate_ids[static_cast<size_t>(y) * 256U + x], motion.id);
        auto const direction = std::find_if(directions.begin(), directions.end(), [&](auto const& candidate) {
            return candidate.plate_id == motion.id;
        });
        ASSERT_NE(direction, directions.end());
        EXPECT_FLOAT_EQ(motion.vector_x, direction->x);
        EXPECT_FLOAT_EQ(motion.vector_y, direction->y);
    }
    for (uint32_t const id : {1U, 6U}) {
        EXPECT_TRUE(std::any_of(image->plate_motions.begin(), image->plate_motions.end(), [id](auto const& motion) {
            return motion.id == id;
        }));
    }
}

TEST(S2MapPresentationTest, V10MapKeepsCanonicalIdentityAndFineClimatePhaseMeaning)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr);
    shared::TerrainWorld world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    client::S2WorldMapState state;
    auto const image = client::rasterizeS2WorldMap(world, *lod, state);
    ASSERT_TRUE(image);
    ASSERT_EQ(image->v10_projected_inland_ice_kind.size(), 256U * 256U);
    ASSERT_EQ(image->v10_river_base_rgba.size(), image->rgba.size());
    // Source-cell stock/palette checks belong to the retained coarse plane.
    // The composed image covers pixel areas with refined standing contacts and
    // nominal rivers, rather than repeating each source point over a whole cell.
    auto const& coarse_rgba = image->v10_river_base_rgba;
    size_t projected_polar_water = 0U;
    size_t expected_projected_polar_water = 0U;
    for (uint32_t y = 0U; y < 64U; ++y) {
        for (uint32_t x = 128U; x < 200U; ++x) {
            size_t const index = static_cast<size_t>(y) * 256U + x;
            auto const phases = generation::sampleV10SurfacePhases(*atlas, x * 256.0, y * 256.0, 0U);
            std::array<double, 8U> seasonal_temperature{};
            for (size_t phase = 0U; phase < phases.size(); ++phase) {
                seasonal_temperature[phase] = phases[phase].temperature_celsius;
            }
            uint8_t const expected_projection = client::s2ProjectedInlandIceKind(seasonal_temperature,
                phases.front().ocean, phases.front().lake, phases.front().river);
            EXPECT_EQ(image->v10_projected_inland_ice_kind[index], expected_projection);
            expected_projected_polar_water += expected_projection != 0U;
            if (image->v10_projected_inland_ice_kind[index] == 0U) continue;
            EXPECT_FALSE(phases.front().ocean);
            EXPECT_TRUE(phases.front().lake || phases.front().river);
            double annual_balance = 0.0;
            for (auto const& phase : phases) annual_balance += phase.temperature_celsius / 8.0;
            EXPECT_LT(annual_balance, 0.0);
            EXPECT_EQ(image->v10_ice_surface_kind[index], 0U);
            if (phases.front().lake)
            {
                size_t const pixel = static_cast<size_t>(y * 2U) * image->width + x * 2U;
                EXPECT_GT(std::min({coarse_rgba[pixel][0U], coarse_rgba[pixel][1U], coarse_rgba[pixel][2U]}), 180U);
            }
            else
            {
                // A nominal river can be subpixel in the whole-world raster.
                // Inspect the same source coordinate rather than widening it to a cell.
                client::MapImageDraw draw;
                draw.source = {(x * 256.0F - 0.125F) / 65'536.0F,
                    (y * 256.0F - 0.125F) / 65'536.0F, 0.25F / 65'536.0F, 0.25F / 65'536.0F};
                draw.destination = {0.0F, 0.0F, 1.0F, 1.0F};
                auto const local = client::rasterizeS2RiverMap(image->v10_river_geometry,
                    std::span{&draw, 1U}, 1U, 1U, 1U, 1U);
                ASSERT_TRUE(local);
                EXPECT_GT(local->v10_river_coverage[0U], 0.0F) << x << ',' << y;
                EXPECT_GT(std::min({local->rgba[0U][0U], local->rgba[0U][1U], local->rgba[0U][2U]}), 180U)
                    << x << ',' << y;
            }
            ++projected_polar_water;
        }
    }
    EXPECT_EQ(projected_polar_water, expected_projected_polar_water);
    RecordProperty("current_footprint_projected_polar_cells", static_cast<int>(projected_polar_water));
    EXPECT_EQ(image->v10_projected_inland_ice_kind[12U * 256U + 185U], client::S2_MAP_PROJECTED_INLAND_ICE);
    // The main lake has a thawing season but a strongly negative annual balance.
    EXPECT_EQ(image->v10_projected_inland_ice_kind[194U], client::S2_MAP_PROJECTED_INLAND_ICE);
    EXPECT_EQ(image->v10_projected_inland_ice_kind[48U * 256U + 173U], 0U);
    EXPECT_EQ(image->v10_projected_inland_ice_kind[48U * 256U + 189U], 0U);
    client::S2WorldMapState biome_state;
    biome_state.layers.fill(false);
    biome_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Biomes)] = true;
    auto const biomes_only = client::rasterizeS2WorldMap(world, *lod, biome_state);
    ASSERT_TRUE(biomes_only);
    size_t snow_cells = 0U;
    for (size_t index = 0U; index < image->v10_ice_surface_kind.size(); ++index) {
        if (image->v10_ice_surface_kind[index] != 1U) continue;
        size_t const pixel = (index / 256U * 2U) * image->width + index % 256U * 2U;
        auto const& covered = coarse_rgba[pixel];
        auto const& biome_covered = biomes_only->rgba[pixel];
        EXPECT_GT(std::min({covered[0U], covered[1U], covered[2U]}), 210U);
        EXPECT_LT(static_cast<int>(covered[1U]) - covered[0U], 12);
        EXPECT_GT(std::min({biome_covered[0U], biome_covered[1U], biome_covered[2U]}), 210U);
        ++snow_cells;
    }
    EXPECT_GT(snow_cells, 0U);
    for (auto const layer : {client::S2WorldMapLayer::Temperature, client::S2WorldMapLayer::Moisture}) {
        auto scalar_state = biome_state;
        scalar_state.layers[static_cast<size_t>(layer)] = true;
        auto const scalar_biomes = client::rasterizeS2WorldMap(world, *lod, scalar_state);
        scalar_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Heights)] = true;
        auto const scalar_heights = client::rasterizeS2WorldMap(world, *lod, scalar_state);
        ASSERT_TRUE(scalar_biomes);
        ASSERT_TRUE(scalar_heights);
        EXPECT_EQ(scalar_biomes->rgba, scalar_heights->rgba);
        EXPECT_EQ(scalar_biomes->layer_ranges, scalar_heights->layer_ranges);
    }
    client::GenerationMapAnimator animator;
    auto const final_frame = animator.updateNative(nullptr, atlas,
        std::make_shared<client::S2MapImage const>(*image), {.reduced_motion = true});
    ASSERT_NE(final_frame.surface_image, nullptr);
    EXPECT_EQ(final_frame.surface_image->rgba, image->rgba);
    EXPECT_EQ(image->source_identity, world.identity());
    EXPECT_EQ(image->source_model_identity, shared::terrainLodModel(generation::TerrainModelTag::V10));
    EXPECT_TRUE(image->v21_elevation_m.empty());
    EXPECT_TRUE(image->v9_elevation_m.empty());
    ASSERT_EQ(image->v10_elevation_m.size(), 256U * 256U);
    // The displayed bed and water color must use the same current V10 query,
    // not the registered native foundation and straight river mask.
    size_t const confluence_index = 118U * 256U + 136U;
    auto const confluence = generation::sampleV10Surface(*atlas, 34'816.0, 30'208.0, 0U);
    ASSERT_TRUE(confluence.river);
    EXPECT_DOUBLE_EQ(image->v10_elevation_m[confluence_index], confluence.elevation_m);
    EXPECT_NEAR(confluence.elevation_m, 4.379'650'743'625'715, 1e-9);
    client::MapImageDraw confluence_draw;
    confluence_draw.source = {(34'816.0F - 0.125F) / 65'536.0F,
        (30'208.0F - 0.125F) / 65'536.0F, 0.25F / 65'536.0F, 0.25F / 65'536.0F};
    confluence_draw.destination = {0.0F, 0.0F, 1.0F, 1.0F};
    auto const local_confluence = client::rasterizeS2RiverMap(image->v10_river_geometry,
        std::span{&confluence_draw, 1U}, 1U, 1U, 1U, 1U);
    ASSERT_TRUE(local_confluence);
    EXPECT_GT(local_confluence->v10_river_coverage[0U], 0.0F);
    EXPECT_EQ(local_confluence->rgba[0U], (std::array<uint8_t, 4U>{35U, 69U, 87U, 255U}));
    auto const expect_surface_pixel = [&](size_t index, generation::V10SurfaceSample const& surface) {
        auto const range = image->layer_ranges[static_cast<size_t>(client::S2WorldMapLayer::Heights)];
        bool const water = surface.ocean || surface.lake;
        auto color = client::s2NativeTerrainColor(static_cast<float>(surface.elevation_m), range,
            {static_cast<float>(image->v10_elevation_m[index / 256U * 256U + (index + 1U) % 256U]),
                static_cast<float>(image->v10_elevation_m[(index + 256U) % (256U * 256U)])},
            water, static_cast<float>(surface.water_level_m - surface.elevation_m), 0U);
        if (!water && !surface.river) color = client::s2NativeBiomeColor(color, surface.biome_id);
        size_t const pixel = index / 256U * 2U * image->width + index % 256U * 2U;
        for (size_t component = 0U; component < 3U; ++component) {
            EXPECT_EQ(coarse_rgba[pixel][component],
                static_cast<uint8_t>(std::clamp(std::lround(color[component]), 0L, 255L)));
        }
    };
    expect_surface_pixel(confluence_index, confluence);
    auto const native_views = generation::v10RegisteredFields(*atlas);
    auto const native_rivers = std::find_if(native_views.begin(), native_views.end(), [](auto const& field) {
        return field.name == "river";
    });
    ASSERT_NE(native_rivers, native_views.end());
    bool changed_footprint_checked = false;
    for (size_t index = 0U; index < 256U * 256U; ++index) {
        bool const native_river = std::visit([index](auto values) { return values[index] != 0; }, native_rivers->values);
        if (!native_river || image->v10_biome_ids[index] == 12U
            || image->v10_ice_surface_kind[index] != 0U || image->v10_projected_inland_ice_kind[index] != 0U) continue;
        auto const surface = generation::sampleV10Surface(*atlas,
            static_cast<double>(index % 256U) * 256.0, static_cast<double>(index / 256U) * 256.0, 0U);
        if (surface.ocean || surface.lake || surface.river) continue;
        expect_surface_pixel(index, surface);
        RecordProperty("curved_vs_native_map_cell", static_cast<int>(index));
        changed_footprint_checked = true;
        break;
    }
    EXPECT_TRUE(changed_footprint_checked);
    ASSERT_EQ(image->v10_land_snow_mean_m.size(), 256U * 256U);
    auto const covered = [](auto const& field) {
        return static_cast<int>(std::count_if(field.begin(), field.end(), [](float value) {
            return value >= 0.1F;
        }));
    };
    RecordProperty("v10_snow_phase0_cells", covered(image->v10_land_snow_phase0_m));
    RecordProperty("v10_snow_mean_cells", covered(image->v10_land_snow_mean_m));
    RecordProperty("v10_glacier_phase0_cells", covered(image->v10_glacier_phase0_m));
    RecordProperty("v10_glacier_mean_cells", covered(image->v10_glacier_mean_m));
    RecordProperty("v10_sea_ice_phase0_cells", covered(image->v10_sea_ice_phase0_m));
    RecordProperty("v10_sea_ice_mean_cells", covered(image->v10_sea_ice_mean_m));
    for (size_t const index : {size_t{0U}, size_t{12'345U}, size_t{43'210U}}) {
        double const x = static_cast<double>(index % 256U) * 256.0;
        double const y = static_cast<double>(index / 256U) * 256.0;
        double temperature = 0.0;
        double snow = 0.0;
        for (uint32_t phase = 0U; phase < 8U; ++phase) {
            auto const sample = generation::sampleV10Surface(*atlas, x, y, 0U, phase);
            EXPECT_DOUBLE_EQ(image->v10_elevation_m[index], sample.elevation_m);
            EXPECT_EQ(image->v10_biome_ids[index], sample.biome_id);
            temperature += sample.temperature_celsius / 8.0;
            snow += sample.snow_water_equivalent_m / 8.0;
            if (phase == 0U) {
                EXPECT_FLOAT_EQ(image->v10_land_snow_phase0_m[index], sample.snow_water_equivalent_m);
                EXPECT_FLOAT_EQ(image->v10_sea_ice_phase0_m[index], sample.sea_ice_thickness_m);
            }
        }
        EXPECT_FLOAT_EQ(image->v10_temperature_mean_c[index], temperature);
        EXPECT_NEAR(image->v10_land_snow_mean_m[index], snow, 1.0e-5);
    }

    static constexpr uint32_t WITNESS_X = 35'840U;
    static constexpr uint32_t WITNESS_Y = 5'120U;
    size_t const witness_index = static_cast<size_t>(WITNESS_Y / 256U) * 256U + WITNESS_X / 256U;
    auto const source = generation::sampleV10AnnualMeanSurface(*atlas, WITNESS_X, WITNESS_Y, 0U);
    ASSERT_FALSE(source.ocean);
    ASSERT_FALSE(source.lake);
    ASSERT_FALSE(source.river);
    EXPECT_EQ(source.biome_id, 3U);
    EXPECT_NEAR(source.temperature_celsius, -8.846, 1e-3);
    EXPECT_NEAR(source.snow_water_equivalent_m, 0.018'859, 1e-6);
    EXPECT_NEAR(source.glacier_thickness_m, 0.005'096, 1e-6);
    EXPECT_EQ(generation::classifyV10AnnualMeanSurfaceCover(source), generation::V10SurfaceCover::Snow);
    ASSERT_EQ(image->v10_ice_surface_kind.size(), 256U * 256U);
    EXPECT_EQ(image->v10_ice_surface_kind[witness_index], 1U);
}

TEST(S2MapPresentationTest, NativeRiverContactPartitionAgreesWithOverlappingDrawUnion)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    auto const image = client::rasterizeS2WorldMap(world, *lod, {});
    ASSERT_TRUE(image);
    ASSERT_NE(image->v10_river_geometry, nullptr);
    for (auto const source : std::array<std::array<float, 4U>, 3U>{{
             {0.448F, 0.378F, 1.0F / 6.0F, 1.0F / 6.0F},
             {0.75F, 0.02F, 0.025F, 0.025F}, {0.97F, 0.2F, 0.03F, 0.03F}}})
    {
        client::MapImageDraw draw;
        draw.source = source;
        draw.destination = {0.0F, 0.0F, 128.0F, 128.0F};
        auto const single = client::rasterizeS2RiverMap(image->v10_river_geometry,
            std::span{&draw, 1U}, 128U, 128U, 128U, 128U);
        std::array const overlapping{draw, draw};
        auto const united = client::rasterizeS2RiverMap(image->v10_river_geometry,
            overlapping, 128U, 128U, 128U, 128U);
        ASSERT_TRUE(single);
        ASSERT_TRUE(united);
        EXPECT_EQ(single->rgba, united->rgba);
        ASSERT_EQ(single->v10_river_coverage.size(), united->v10_river_coverage.size());
        for (size_t i = 0U; i < single->v10_river_coverage.size(); ++i)
        {
            EXPECT_NEAR(single->v10_river_coverage[i], united->v10_river_coverage[i], 1.0e-7F);
        }
    }

    client::S2MapLayout first;
    first.world_draw_count = 1U;
    first.world_draws[0].destination = {0.0F, 0.0F, 128.0F, 128.0F};
    client::S2MapLayout latest = first;
    latest.world_draws[0].source = {0.448F, 0.378F, 1.0F / 6.0F, 1.0F / 6.0F};
    client::S2WorldMapRiverWorker worker;
    EXPECT_FALSE(worker.update(image->v10_river_geometry, first, 128U, 128U));
    EXPECT_FALSE(worker.update(image->v10_river_geometry, latest, 128U, 128U));

    std::shared_ptr<client::S2MapImage const> result;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    do {
        result = worker.update(image->v10_river_geometry, latest, 128U, 128U);
        if (!result) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    } while (!result && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(result);
    auto const exact = client::rasterizeS2RiverMap(image->v10_river_geometry,
        std::span{latest.world_draws.data(), 1U}, 128U, 128U, 128U, 128U);
    ASSERT_TRUE(exact);
    EXPECT_EQ(result->rgba, exact->rgba);
    worker.invalidate();
    EXPECT_FALSE(worker.update(image->v10_river_geometry, latest, 128U, 128U));
}

TEST(S2MapPresentationTest, NativeRiverBanksReachTheQueriedOceanContact)
{
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    auto const image = client::rasterizeS2WorldMap(world, *lod, {});
    ASSERT_TRUE(image);
    auto const local = [&](double x, double y) {
        client::MapImageDraw draw;
        draw.source = {static_cast<float>((x - 0.125) / 65'536.0),
            static_cast<float>((y - 0.125) / 65'536.0), 0.25F / 65'536.0F, 0.25F / 65'536.0F};
        draw.destination = {0.0F, 0.0F, 1.0F, 1.0F};
        return client::rasterizeS2RiverMap(image->v10_river_geometry, std::span{&draw, 1U}, 1U, 1U, 1U, 1U);
    };
    for (double x : {34'847.0, 34'849.0})
    {
        auto const source = generation::sampleV10AnnualMeanSurface(*atlas, x, 30'204.0, 0U);
        ASSERT_TRUE(source.river);
        ASSERT_FALSE(source.ocean);
        ASSERT_LT(source.elevation_m, source.water_level_m);
        auto const mask = local(x, 30'204.0);
        ASSERT_TRUE(mask);
        EXPECT_GT(mask->v10_river_coverage[0U], 0.99F);
    }
    // The whole control pixel is ocean; a point on the shoreline would have
    // legitimate partial river area even when its center is already ocean.
    for (double dx : {-0.125, 0.125})
    {
        for (double dy : {-0.125, 0.125})
        {
            auto const sea = generation::sampleV10AnnualMeanSurface(*atlas, 34'854.0 + dx, 30'204.0 + dy, 0U);
            ASSERT_TRUE(sea.ocean);
        }
    }
    auto const sea_mask = local(34'854.0, 30'204.0);
    ASSERT_TRUE(sea_mask);
    EXPECT_EQ(sea_mask->v10_river_coverage[0U], 0.0F);
    auto const land = generation::sampleV10AnnualMeanSurface(*atlas, 34'860.0, 30'200.0, 0U);
    ASSERT_FALSE(land.ocean || land.lake || land.river);
    auto const land_mask = local(34'860.0, 30'200.0);
    ASSERT_TRUE(land_mask);
    EXPECT_GT(land_mask->rgba[0U][1U], land_mask->rgba[0U][2U]);
}

// Opt-in CPU frames for visual inspection; these are observations, never golden approval.
TEST(S2MapPresentationTest, DISABLED_CaptureCurrentRiverGeometryViews)
{
    using Clock = std::chrono::steady_clock;
    auto const elapsed_ms = [](Clock::time_point begin) {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    char const* directory = std::getenv("MC_RIVER_MAP_EVIDENCE");
    ASSERT_NE(directory, nullptr);
    std::filesystem::create_directories(directory);
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto begin = Clock::now();
    auto const atlas = generation::prepareAtlas(configuration);
    RecordProperty("atlas_prepare_ms", std::to_string(elapsed_ms(begin)));
    shared::TerrainWorld const world(atlas, 17U);
    begin = Clock::now();
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    RecordProperty("l0_prepare_ms", std::to_string(elapsed_ms(begin)));
    client::S2WorldMapState state;
    begin = Clock::now();
    auto const image = client::rasterizeS2WorldMap(world, *lod, state);
    RecordProperty("map_prepare_ms", std::to_string(elapsed_ms(begin)));
    ASSERT_TRUE(image);
    ASSERT_NE(image->v10_river_geometry, nullptr);
    auto const first_open_layout = client::layoutS2Map(state, 1920U, 1152U, true, false);
    begin = Clock::now();
    auto const first_open = client::composeS2WorldMapView(&*image, first_open_layout, {}, 1920U, 1152U);
    RecordProperty("desktop_first_open_ms", std::to_string(elapsed_ms(begin)));
    ASSERT_TRUE(first_open);
    begin = Clock::now();
    auto const immediate = client::composeS2WorldMapViewWithRiverLayer(
        &*image, first_open_layout, {}, 1920U, 1152U, nullptr);
    RecordProperty("desktop_baked_first_open_ms", std::to_string(elapsed_ms(begin)));
    ASSERT_TRUE(immediate);
    struct View
    {
        char const* name;
        double x, y, span;
    };
    for (auto const& view : std::array<View, 4U>{{{"overview", 32'768.0, 32'768.0, 65'536.0},
             {"zoom6-confluence", 34'816.0, 30'208.0, 65'536.0 / 6.0},
             {"diagnostic-outlet", 34'850.0, 30'208.0, 128.0},
             {"diagnostic-cold", 50'048.0, 2000.0, 512.0}}})
    {
        client::S2MapLayout layout;
        layout.world_draw_count = 1U;
        auto& draw = layout.world_draws[0U];
        draw.source = {static_cast<float>((view.x - view.span * 0.5) / 65'536.0),
            static_cast<float>((view.y - view.span * 0.5) / 65'536.0),
            static_cast<float>(view.span / 65'536.0), static_cast<float>(view.span / 65'536.0)};
        draw.destination = {0.0F, 0.0F, 512.0F, 512.0F};
        begin = Clock::now();
        auto const composed = client::composeS2WorldMapView(&*image, layout, {}, 512U, 512U);
        RecordProperty(std::string(view.name) + "_first_compose_ms", std::to_string(elapsed_ms(begin)));
        ASSERT_TRUE(composed);
        begin = Clock::now();
        auto const cached = client::composeS2WorldMapView(&*image, layout, {}, 512U, 512U);
        RecordProperty(std::string(view.name) + "_cached_compose_ms", std::to_string(elapsed_ms(begin)));
        ASSERT_TRUE(cached);
        EXPECT_EQ(cached->rgba, composed->rgba);
        auto panned_layout = layout;
        panned_layout.world_draws[0U].source[0U] += draw.source[2U] / 512.0F;
        if (panned_layout.world_draws[0U].source[0U] + draw.source[2U] <= 1.0F)
        {
            begin = Clock::now();
            auto const panned = client::composeS2WorldMapView(&*image, panned_layout, {}, 512U, 512U);
            RecordProperty(std::string(view.name) + "_panned_compose_ms", std::to_string(elapsed_ms(begin)));
            ASSERT_TRUE(panned);
        }
        std::ofstream output(std::string(directory) + "/" + view.name + ".ppm", std::ios::binary);
        output << "P6\n512 512\n255\n";
        for (auto const& pixel : composed->rgba)
        {
            output.write(reinterpret_cast<char const*>(pixel.data()), 3);
        }
        ASSERT_TRUE(output);
    }
}

TEST(S2MapPresentationTest, DISABLED_CaptureRigidWeatherTemporalFrames)
{
    constexpr uint32_t WIDTH = 1280U;
    constexpr uint32_t HEIGHT = 676U;
    auto atlas = std::make_shared<generation::GlobalAtlas>(*mapWorld(true, true)->atlas());
    for (uint32_t y = 0U; y < 256U; ++y) {
        for (uint32_t x = 0U; x < 256U; ++x) {
            size_t const index = static_cast<size_t>(y) * 256U + x;
            atlas->ocean_mask[index] = x >= 128U;
            atlas->water_level[index] = x >= 128U ? 2.0F : 0.0F;
            float const angle = (static_cast<float>(y) / 255.0F - 0.5F) * 1.2F;
            atlas->wind_u[index] = std::cos(angle);
            atlas->wind_v[index] = std::sin(angle);
            atlas->current_u[index] = 0.8F;
            atlas->current_v[index] = -0.3F;
        }
    }
    shared::TerrainWorld world(atlas);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    for (bool const current : {false, true}) {
        client::S2WorldMapState state;
        state.layers.fill(false);
        state.layers[static_cast<size_t>(current ? client::S2WorldMapLayer::Currents
            : client::S2WorldMapLayer::Wind)] = true;
        auto const image = client::rasterizeS2WorldMap(world, *lod, state);
        ASSERT_TRUE(image);
        for (uint32_t frame = 0U; frame < 4U; ++frame) {
            std::vector<client::GuiVectorVertex> triangles;
            client::TextRenderer labels;
            static_cast<void>(client::buildS2WorldMapOverlay(state, &*image, &world,
                32'768.0, 32'768.0, WIDTH, HEIGHT, 1.0F, labels, triangles,
                std::array<float, 4U>{0.0F, 1.0F, 2.0F, 4.0F}[frame]));
            std::vector<std::array<uint8_t, 3U>> pixels(static_cast<size_t>(WIDTH) * HEIGHT);
            for (uint32_t y = 0U; y < HEIGHT; ++y) {
                for (uint32_t x = 0U; x < WIDTH; ++x) {
                    pixels[static_cast<size_t>(y) * WIDTH + x] = x < WIDTH / 2U
                        ? std::array<uint8_t, 3U>{65U, 83U, 59U}
                        : std::array<uint8_t, 3U>{29U, 65U, 84U};
                }
            }
            auto const edge = [](auto const& a, auto const& b, float x, float y) {
                return (x - a[0U]) * (b[1U] - a[1U]) - (y - a[1U]) * (b[0U] - a[0U]);
            };
            for (size_t index = 0U; index + 2U < triangles.size(); index += 3U) {
                auto const& a = triangles[index];
                auto const& b = triangles[index + 1U];
                auto const& c = triangles[index + 2U];
                float const area = edge(a.position, b.position, c.position[0U], c.position[1U]);
                if (std::abs(area) < 1.0e-5F) continue;
                int const left = std::max(0,
                    static_cast<int>(std::floor(std::min({a.position[0U], b.position[0U], c.position[0U]}))));
                int const right = std::min(static_cast<int>(WIDTH) - 1,
                    static_cast<int>(std::ceil(std::max({a.position[0U], b.position[0U], c.position[0U]}))));
                int const top = std::max(0,
                    static_cast<int>(std::floor(std::min({a.position[1U], b.position[1U], c.position[1U]}))));
                int const bottom = std::min(static_cast<int>(HEIGHT) - 1,
                    static_cast<int>(std::ceil(std::max({a.position[1U], b.position[1U], c.position[1U]}))));
                for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) {
                    float const px = static_cast<float>(x) + 0.5F;
                    float const py = static_cast<float>(y) + 0.5F;
                    float const wa = edge(b.position, c.position, px, py) / area;
                    float const wb = edge(c.position, a.position, px, py) / area;
                    float const wc = 1.0F - wa - wb;
                    if (wa < 0.0F || wb < 0.0F || wc < 0.0F) continue;
                    float const alpha = (wa * a.rgba[3U] + wb * b.rgba[3U] + wc * c.rgba[3U]) / 255.0F;
                    auto& pixel = pixels[static_cast<size_t>(y) * WIDTH + static_cast<size_t>(x)];
                    for (size_t channel = 0U; channel < 3U; ++channel) {
                        float const color = wa * a.rgba[channel] + wb * b.rgba[channel] + wc * c.rgba[channel];
                        pixel[channel] = static_cast<uint8_t>(std::lround(pixel[channel] * (1.0F - alpha) + color * alpha));
                    }
                }
            }
            std::string const filename = std::string("/tmp/mc-s2-weather-")
                + (current ? "current-" : "wind-") + std::to_string(frame) + ".ppm";
            std::ofstream output(filename, std::ios::binary);
            ASSERT_TRUE(output);
            output << "P6\n" << WIDTH << ' ' << HEIGHT << "\n255\n";
            output.write(reinterpret_cast<char const*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 3U));
            ASSERT_TRUE(output);
        }
    }
}

TEST(S2MapPresentationTest, WeatherTrailSegmentScreenLengthStaysConstantAcrossWorldZoom)
{
    auto const world = mapWorld(true, true);
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Wind)] = true;
    auto const image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    for (double const zoom : {1.0, 2.0, 4.0}) {
        state.zoom = zoom;
        client::TextRenderer labels;
        std::vector<client::GuiVectorVertex> triangles;
        static_cast<void>(client::buildS2WorldMapOverlay(state, &*image, world.get(),
            32'768.0, 32'768.0, 1'280U, 676U, 1.0F, labels, triangles, 0.37F));
        float longest_arrow_edge = 0.0F;
        size_t arrow_triangles = 0U;
        for (size_t index = 0U; index + 2U < triangles.size(); index += 3U) {
            auto const arrow_vertex = [](auto const& vertex) {
                return vertex.rgba[0U] == 62U && vertex.rgba[1U] == 232U && vertex.rgba[2U] == 216U;
            };
            if (!arrow_vertex(triangles[index]) || !arrow_vertex(triangles[index + 1U])
                || !arrow_vertex(triangles[index + 2U])) continue;
            ++arrow_triangles;
            for (size_t edge = 0U; edge < 3U; ++edge) {
                auto const& a = triangles[index + edge].position;
                auto const& b = triangles[index + (edge + 1U) % 3U].position;
                longest_arrow_edge = std::max(longest_arrow_edge, std::hypot(a[0U] - b[0U], a[1U] - b[1U]));
            }
        }
        EXPECT_GT(arrow_triangles, 0U);
        EXPECT_NEAR(longest_arrow_edge, 6.0F, 0.3F) << "zoom=" << zoom;
    }
}

TEST(S2MapPresentationTest, RenderedMapActionsHaveExactBoundedNonoverlappingHitRegions)
{
    using Action = client::S2WorldMapAction;
    constexpr std::array<Action, 11U> ACTIONS{
        Action::Overview, Action::Reset, Action::ToggleLegend,
        Action::TogglePlates, Action::ToggleHeights, Action::ToggleTemperature, Action::ToggleBiomes,
        Action::ToggleWind, Action::ToggleCurrents, Action::ToggleMoisture, Action::TogglePrecipitation,
    };
    constexpr std::array<std::string_view, 11U> DESKTOP_LABELS{
        "0 Whole world", "R Reset", "L Details", "P Plate OFF", "H Height OFF", "T Temp OFF", "B Biomes OFF",
        "Ctrl+W Wind OFF", "C Curr OFF", "O Moist OFF", "Q Rain OFF",
    };
    constexpr std::array<std::string_view, 11U> TOUCH_LABELS{
        "Whole world", "Reset", "Details", "Plate OFF", "Height OFF", "Temp OFF", "Biomes OFF",
        "Wind OFF", "Curr OFF", "Moist OFF", "Rain OFF",
    };
    struct Viewport final {
        uint32_t width;
        uint32_t height;
        float dpi;
        bool touch;
    };
    for (Viewport const viewport : {Viewport{1920U, 1080U, 1.0F, false}, Viewport{2400U, 1080U, 2.625F, true}}) {
        client::S2WorldMapState state;
        state.layers.fill(false);
        client::S2MapImage image;
        image.available_layers.fill(true);
        client::TextRenderer labels;
        std::vector<client::GuiVectorVertex> triangles;
        auto const layout = client::buildS2WorldMapOverlay(state, &image, nullptr, 0.0, 0.0,
            viewport.width, viewport.height, viewport.dpi, labels, triangles, 0.0F, viewport.touch);
        ASSERT_EQ(layout.action_region_count, ACTIONS.size());
        auto const hit = [&](float const x, float const y) -> std::optional<Action> {
            for (uint32_t index = 0U; index < layout.action_region_count; ++index) {
                auto const& region = layout.action_regions[index];
                auto const& b = region.bounds;
                if (x >= b[0U] && y >= b[1U] && x < b[0U] + b[2U] && y < b[1U] + b[3U]) {
                    return region.action;
                }
            }
            return std::nullopt;
        };
        for (size_t index = 0U; index < ACTIONS.size(); ++index) {
            auto const& region = layout.action_regions[index];
            auto const& b = region.bounds;
            EXPECT_EQ(region.action, ACTIONS[index]);
            EXPECT_GE(b[0U], 0.0F);
            EXPECT_GE(b[1U], 0.0F);
            EXPECT_LE(b[0U] + b[2U], viewport.width);
            EXPECT_LE(b[1U] + b[3U], viewport.height);
            EXPECT_GT(b[2U], 0.0F);
            EXPECT_GT(b[3U], 0.0F);
            if (viewport.touch) {
                EXPECT_GE(b[2U], 48.0F * viewport.dpi);
                EXPECT_GE(b[3U], 48.0F * viewport.dpi);
            }
            EXPECT_EQ(hit(b[0U], b[1U]), ACTIONS[index]);
            EXPECT_EQ(hit(b[0U] + b[2U] * 0.5F, b[1U] + b[3U] * 0.5F), ACTIONS[index]);
            EXPECT_NE(hit(b[0U] + b[2U], b[1U]), ACTIONS[index]);
            EXPECT_NE(hit(b[0U], b[1U] + b[3U]), ACTIONS[index]);
            auto const& expected = viewport.touch ? TOUCH_LABELS : DESKTOP_LABELS;
            auto const label = std::find_if(labels.labels().begin(), labels.labels().end(), [&](auto const& text) {
                float const text_x = b[0U] + (viewport.touch ? 8.0F : 0.0F) * viewport.dpi;
                float const text_y = b[1U] + (viewport.touch ? 16.0F : 2.0F) * viewport.dpi;
                return text.value == expected[index] && text.placement.position[0U] == text_x
                    && text.placement.position[1U] == text_y;
            });
            ASSERT_NE(label, labels.labels().end());
            EXPECT_LE(label->placement.position[0U] + static_cast<float>(label->value.size()) * 8.0F * viewport.dpi,
                b[0U] + b[2U]);
            EXPECT_LE(label->placement.position[1U] + 16.0F * viewport.dpi, b[1U] + b[3U]);
            for (size_t other = index + 1U; other < ACTIONS.size(); ++other) {
                auto const& c = layout.action_regions[other].bounds;
                EXPECT_TRUE(b[0U] + b[2U] <= c[0U] || c[0U] + c[2U] <= b[0U]
                    || b[1U] + b[3U] <= c[1U] || c[1U] + c[3U] <= b[1U]);
            }
        }
    }
}

TEST(S2MapPresentationTest, SlowSubpixelPanInterpolatesRatherThanSwitchingWholeSourceCells)
{
    client::S2MapImage source{.width = 2U, .height = 1U};
    source.rgba = {{0U, 0U, 0U, 255U}, {255U, 255U, 255U, 255U}};
    client::S2MapLayout layout;
    layout.world_draw_count = 1U;
    layout.world_draws[0U] = {
        .image = 0U, .source = {0.0F, 0.0F, .98F, 1.0F},
        .destination = {0.0F, 0.0F, 100.0F, 1.0F},
    };
    auto const before = client::composeS2WorldMapView(&source, layout, {}, 100U, 1U);
    layout.world_draws[0U].source[0U] += .01F;
    auto const after = client::composeS2WorldMapView(&source, layout, {}, 100U, 1U);
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    int const change = static_cast<int>(after->rgba[50U][0U]) - before->rgba[50U][0U];
    EXPECT_GT(change, 0);
    EXPECT_LE(change, 10);
}

TEST(S2MapPresentationTest, PlateJunctionSplitsEveryIncidentOwnershipRun)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>(*mapWorld()->atlas());
    for (uint32_t y = 0U; y < 256U; ++y) {
        for (uint32_t x = 0U; x < 256U; ++x) {
            atlas->plate_id[static_cast<size_t>(y) * 256U + x] = x < 128U ? 1 : (y < 128U ? 2 : 3);
        }
    }
    shared::TerrainWorld world(std::move(atlas));
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto const image = client::rasterizeS2WorldMap(world, *lod, state);
    ASSERT_TRUE(image);
    std::array<float, 2U> const junction{.5F, .5F};
    size_t const degree = static_cast<size_t>(std::count_if(image->plate_boundaries.begin(),
        image->plate_boundaries.end(), [&](auto const& edge) {
            return edge.start == junction || edge.end == junction;
        }));
    EXPECT_EQ(degree, 3U);
}

TEST(S2MapPresentationTest, ContinuousWeatherTrailsTravelTwentyLogicalPixelsPerSecond)
{
    std::vector<float> const u(64U, 1.0F);
    std::vector<float> const v(64U, 0.0F);
    client::S2MapWeatherFields const fields{.resolution = 8U, .wind_u = u, .wind_v = v};
    client::S2MapLayout layout;
    layout.world_draw_count = 1U;
    layout.world_draws[0U].destination = {0.0F, 0.0F, 1'024.0F, 1'024.0F};
    auto visible_points = [&](float seconds) {
        std::vector<client::GuiVectorVertex> vertices;
        client::appendS2MapWeatherTriangles(vertices, layout, fields, seconds, 1.0F, true, false, false);
        std::vector<std::array<float, 2U>> points;
        for (auto const& vertex : vertices) {
            // Compare fully visible interior geometry; fades and border clipping
            // can change the set of vertices without changing the travel rate.
            if (vertex.rgba == (std::array<uint8_t, 4U>{62U, 232U, 216U, 255U})
                && vertex.position[0U] > 100.0F && vertex.position[0U] < 900.0F
                && vertex.position[1U] > 100.0F && vertex.position[1U] < 900.0F) {
                points.push_back(vertex.position);
            }
        }
        return points;
    };
    auto const before = visible_points(0.0F);
    auto const after = visible_points(.1F);
    size_t translated_vertices = 0U;
    for (auto const point : before) {
        translated_vertices += std::any_of(after.begin(), after.end(), [&](auto const next) {
            return std::abs(next[1U] - point[1U]) < .001F
                && std::abs(next[0U] - point[0U] - 2.0F) < .001F;
        }) ? 1U : 0U;
    }
    EXPECT_GT(translated_vertices, 768U);
}

TEST(S2MapPresentationTest, RainImpactsIgnoreHorizontalWind)
{
    std::vector<float> rain(64U, 1.0F);
    std::vector<float> u(64U, 0.0F);
    std::vector<float> v(64U, 0.0F);
    client::S2MapWeatherFields fields{
        .resolution = 8U, .precipitation_resolution = 8U,
        .wind_u = u, .wind_v = v, .precipitation = rain, .precipitation_maximum = 1.0F,
    };
    client::S2MapLayout layout;
    layout.world_draw_count = 1U;
    layout.world_draws[0U].destination = {0.0F, 0.0F, 512.0F, 512.0F};
    std::vector<client::GuiVectorVertex> calm;
    client::appendS2MapRainTriangles(calm, layout, fields, .2F);
    ASSERT_FALSE(calm.empty());
    std::fill(u.begin(), u.end(), 30.0F);
    std::fill(v.begin(), v.end(), -20.0F);
    std::vector<client::GuiVectorVertex> windy;
    client::appendS2MapRainTriangles(windy, layout, fields, .2F);
    ASSERT_EQ(calm.size(), windy.size());
    for (size_t index = 0U; index < calm.size(); ++index) {
        EXPECT_EQ(calm[index].position, windy[index].position);
        EXPECT_EQ(calm[index].rgba, windy[index].rgba);
    }
    std::fill(rain.begin(), rain.end(), 0.0F);
    std::vector<client::GuiVectorVertex> dry;
    client::appendS2MapRainTriangles(dry, layout, fields, .2F);
    EXPECT_TRUE(dry.empty());
}

TEST(S2MapPresentationTest, RainZoomKeepsDropsLegibleAndAddsSpatialDetail)
{
    std::vector<float> rain(64U, 1.0F);
    client::S2MapWeatherFields const fields{
        .precipitation_resolution = 8U, .precipitation = rain, .precipitation_maximum = 1.0F,
    };
    size_t normal_count = 0U;
    for (float const side : {128.0F, 512.0F, 1'024.0F, 4'096.0F}) {
        client::S2MapLayout layout;
        layout.world_draw_count = 1U;
        layout.world_draws[0U].destination = {0.0F, 0.0F, side, side};
        std::vector<client::GuiVectorVertex> vertices;
        client::appendS2MapRainTriangles(vertices, layout, fields, .2F);
        ASSERT_GE(vertices.size(), 3U);
        // The first solid stroke's end corners expose its visible width.
        float const width = std::hypot(vertices[1U].position[0U] - vertices[2U].position[0U],
            vertices[1U].position[1U] - vertices[2U].position[1U]);
        EXPECT_GE(width, 1.8F);
        EXPECT_LE(width, 4.4F);
        if (side == 512.0F) {
            normal_count = vertices.size();
        }
    }
    client::S2MapLayout close;
    close.world_draw_count = 1U;
    close.world_draws[0U].destination = {0.0F, 0.0F, 512.0F, 512.0F};
    close.world_draws[0U].source = {.25F, .25F, .125F, .125F};
    std::vector<client::GuiVectorVertex> zoomed;
    client::appendS2MapRainTriangles(zoomed, close, fields, .2F);
    EXPECT_GT(zoomed.size(), normal_count / 8U);
}

TEST(S2MapPresentationTest, RainAmountChangesEventFrequencyRatherThanFallDistance)
{
    std::vector<float> rain(64U, .05F);
    client::S2MapWeatherFields fields{
        .precipitation_resolution = 8U, .precipitation = rain, .precipitation_maximum = 1.0F,
    };
    client::S2MapLayout layout;
    layout.world_draw_count = 1U;
    layout.world_draws[0U].destination = {0.0F, 0.0F, 512.0F, 512.0F};
    auto visible_count = [&] {
        size_t count = 0U;
        for (uint32_t frame = 0U; frame < 13U; ++frame) {
            std::vector<client::GuiVectorVertex> vertices;
            client::appendS2MapRainTriangles(vertices, layout, fields, frame * .05F);
            count += vertices.size();
        }
        return count;
    };
    size_t const light = visible_count();
    std::fill(rain.begin(), rain.end(), 1.0F);
    size_t const heavy = visible_count();
    EXPECT_GT(light, 0U);
    EXPECT_GT(heavy, light * 2U);
}

namespace {

bool writeWeatherCpuCapture(
    std::span<client::GuiVectorVertex const> triangles, uint32_t side, std::string const& filename
)
{
    std::vector<std::array<uint8_t, 3U>> pixels(static_cast<size_t>(side) * side);
    for (uint32_t y = 0U; y < side; ++y) {
        for (uint32_t x = 0U; x < side; ++x) {
            pixels[static_cast<size_t>(y) * side + x] = x < side / 2U
                ? std::array<uint8_t, 3U>{65U, 83U, 59U}
                : std::array<uint8_t, 3U>{29U, 65U, 84U};
        }
    }
    auto const edge = [](auto const& a, auto const& b, float x, float y) {
        return (x - a[0U]) * (b[1U] - a[1U]) - (y - a[1U]) * (b[0U] - a[0U]);
    };
    for (size_t index = 0U; index + 2U < triangles.size(); index += 3U) {
        auto const& a = triangles[index];
        auto const& b = triangles[index + 1U];
        auto const& c = triangles[index + 2U];
        float const area = edge(a.position, b.position, c.position[0U], c.position[1U]);
        if (std::abs(area) < 1.0e-5F) continue;
        int const left = std::max(0,
            static_cast<int>(std::floor(std::min({a.position[0U], b.position[0U], c.position[0U]}))));
        int const right = std::min(static_cast<int>(side) - 1,
            static_cast<int>(std::ceil(std::max({a.position[0U], b.position[0U], c.position[0U]}))));
        int const top = std::max(0,
            static_cast<int>(std::floor(std::min({a.position[1U], b.position[1U], c.position[1U]}))));
        int const bottom = std::min(static_cast<int>(side) - 1,
            static_cast<int>(std::ceil(std::max({a.position[1U], b.position[1U], c.position[1U]}))));
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) {
            float const px = static_cast<float>(x) + 0.5F;
            float const py = static_cast<float>(y) + 0.5F;
            float const wa = edge(b.position, c.position, px, py) / area;
            float const wb = edge(c.position, a.position, px, py) / area;
            float const wc = 1.0F - wa - wb;
            if (wa < 0.0F || wb < 0.0F || wc < 0.0F) continue;
            float const alpha = (wa * a.rgba[3U] + wb * b.rgba[3U] + wc * c.rgba[3U]) / 255.0F;
            auto& pixel = pixels[static_cast<size_t>(y) * side + static_cast<size_t>(x)];
            for (size_t channel = 0U; channel < 3U; ++channel) {
                float const color = wa * a.rgba[channel] + wb * b.rgba[channel] + wc * c.rgba[channel];
                pixel[channel] = static_cast<uint8_t>(std::lround(pixel[channel] * (1.0F - alpha) + color * alpha));
            }
        }
    }
    std::ofstream output(filename, std::ios::binary);
    if (!output) return false;
    output << "P6\n" << side << ' ' << side << "\n255\n";
    output.write(reinterpret_cast<char const*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 3U));
    return static_cast<bool>(output);
}

bool writeMapCpuCapture(
    client::S2MapImage const& world_image,
    client::S2MapLayout const& map_layout,
    client::S2MapOverlayLayout const& overlay_layout,
    std::span<client::GuiVectorVertex const> triangles,
    uint32_t const width,
    uint32_t const height,
    std::string const& filename
)
{
    auto const view = client::composeS2WorldMapView(
        &world_image, map_layout, overlay_layout, width, height);
    if (!view) return false;
    std::vector<std::array<uint8_t, 3U>> pixels(view->rgba.size());
    for (size_t index = 0U; index < view->rgba.size(); ++index) {
        pixels[index] = {view->rgba[index][0U], view->rgba[index][1U], view->rgba[index][2U]};
    }
    float const scale_x = static_cast<float>(view->width) / static_cast<float>(width);
    float const scale_y = static_cast<float>(view->height) / static_cast<float>(height);
    auto const position = [&](client::GuiVectorVertex const& vertex) {
        return std::array<float, 2U>{vertex.position[0U] * scale_x, vertex.position[1U] * scale_y};
    };
    auto const edge = [](auto const& a, auto const& b, float const x, float const y) {
        return (x - a[0U]) * (b[1U] - a[1U]) - (y - a[1U]) * (b[0U] - a[0U]);
    };
    for (size_t index = 0U; index + 2U < triangles.size(); index += 3U) {
        auto const a = position(triangles[index]);
        auto const b = position(triangles[index + 1U]);
        auto const c = position(triangles[index + 2U]);
        float const area = edge(a, b, c[0U], c[1U]);
        if (std::abs(area) < 1.0e-5F) continue;
        int const left = std::max(0, static_cast<int>(std::floor(std::min({a[0U], b[0U], c[0U]}))));
        int const right = std::min(static_cast<int>(view->width) - 1,
            static_cast<int>(std::ceil(std::max({a[0U], b[0U], c[0U]}))));
        int const top = std::max(0, static_cast<int>(std::floor(std::min({a[1U], b[1U], c[1U]}))));
        int const bottom = std::min(static_cast<int>(view->height) - 1,
            static_cast<int>(std::ceil(std::max({a[1U], b[1U], c[1U]}))));
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) {
            float const px = static_cast<float>(x) + 0.5F;
            float const py = static_cast<float>(y) + 0.5F;
            float const wa = edge(b, c, px, py) / area;
            float const wb = edge(c, a, px, py) / area;
            float const wc = 1.0F - wa - wb;
            if (wa < 0.0F || wb < 0.0F || wc < 0.0F) continue;
            float const alpha = (wa * triangles[index].rgba[3U]
                + wb * triangles[index + 1U].rgba[3U] + wc * triangles[index + 2U].rgba[3U]) / 255.0F;
            auto& pixel = pixels[static_cast<size_t>(y) * view->width + static_cast<size_t>(x)];
            for (size_t channel = 0U; channel < 3U; ++channel) {
                float const color = wa * triangles[index].rgba[channel]
                    + wb * triangles[index + 1U].rgba[channel] + wc * triangles[index + 2U].rgba[channel];
                pixel[channel] = static_cast<uint8_t>(std::lround(
                    static_cast<float>(pixel[channel]) * (1.0F - alpha) + color * alpha));
            }
        }
    }
    std::ofstream output(filename, std::ios::binary);
    if (!output) return false;
    output << "P6\n" << view->width << ' ' << view->height << "\n255\n";
    output.write(reinterpret_cast<char const*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size() * 3U));
    return static_cast<bool>(output);
}

} // namespace

TEST(S2MapPresentationTest, DISABLED_CaptureP16MatchedV10PlateViews)
{
    char const* directory = std::getenv("MC_P16_MAP_CAPTURE_DIR");
    ASSERT_NE(directory, nullptr);
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr);
    shared::TerrainWorld world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);

    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto const image = client::rasterizeS2WorldMap(world, *lod, state);
    ASSERT_TRUE(image);
    ASSERT_EQ(image->source_identity, world.identity());
    ASSERT_EQ(image->source_model_identity, shared::terrainLodModel(generation::TerrainModelTag::V10));
    auto const plate_ids = generation::v10CurrentPlateIds(*atlas);
    ASSERT_EQ(plate_ids.size(), 256U * 256U);
    auto sorted_plate_ids = std::vector<uint32_t>(plate_ids.begin(), plate_ids.end());
    std::sort(sorted_plate_ids.begin(), sorted_plate_ids.end());
    sorted_plate_ids.erase(std::unique(sorted_plate_ids.begin(), sorted_plate_ids.end()), sorted_plate_ids.end());
    uint64_t plate_id_hash = 14'695'981'039'346'656'037ULL;
    for (uint32_t const id : plate_ids) {
        for (uint32_t shift = 0U; shift < 32U; shift += 8U) {
            plate_id_hash ^= (id >> shift) & 0xffU;
            plate_id_hash *= 1'099'511'628'211ULL;
        }
    }
    auto const plate3 = std::find_if(image->plate_motions.begin(), image->plate_motions.end(), [](auto const& plate) {
        return plate.id == 3U;
    });
    ASSERT_NE(plate3, image->plate_motions.end());
    double const center_x = static_cast<double>(plate3->x) * generation::WORLD_SIZE_BLOCKS;
    double const center_y = static_cast<double>(plate3->y) * generation::WORLD_SIZE_BLOCKS;
    uint32_t constexpr WIDTH = 640U;
    uint32_t constexpr HEIGHT = 480U;
    std::ofstream manifest(std::string(directory) + "/manifest.txt");
    ASSERT_TRUE(manifest);
    manifest << "renderer=buildS2WorldMapOverlay+composeS2WorldMapView+VulkanOffscreenRenderer\n"
        << "model=V10 terrain_version=" << world.identity().terrain_version
        << " revision=" << world.identity().revision << " seed=" << world.identity().seed
        << " configuration_identity=" << world.identity().configuration_identity << '\n'
        << "source_model_identity=" << image->source_model_identity
        << " source_plate_ids=" << plate_ids.size() << " unique_plate_ids=" << sorted_plate_ids.size()
        << " plate_id_fnv1a64=" << plate_id_hash << '\n'
        << "plate3_center_u=" << plate3->x << " center_v=" << plate3->y
        << " center_x_blocks=" << center_x << " center_y_blocks=" << center_y
        << " motion_x=" << plate3->vector_x << " motion_y=" << plate3->vector_y << '\n'
        << "framebuffer=" << WIDTH << 'x' << HEIGHT << " dpi=1 layers=plates-only\n";

    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, true};
    renderer.setDebugHudEnabled(false);
    ASSERT_TRUE(renderer.validationEnabled());

    for (float const zoom : {5.35F, 6.0F}) {
        state.center_x = center_x;
        state.center_y = center_y;
        state.zoom = zoom;
        state.overview = false;
        auto const map_layout = client::layoutS2Map(state, WIDTH, HEIGHT, true, false, 1.0F);
        ASSERT_GT(map_layout.world_draw_count, 0U);
        auto boundary_image = *image;
        boundary_image.plate_motions.clear();
        client::TextRenderer boundary_text;
        std::vector<client::GuiVectorVertex> boundary_triangles;
        auto const boundary_overlay = client::buildS2WorldMapOverlay(state, &boundary_image, &world,
            std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(),
            WIDTH, HEIGHT, 1.0F, boundary_text, boundary_triangles);
        client::TextRenderer full_text;
        std::vector<client::GuiVectorVertex> full_triangles;
        auto const full_overlay = client::buildS2WorldMapOverlay(state, &*image, &world,
            std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(),
            WIDTH, HEIGHT, 1.0F, full_text, full_triangles);
        ASSERT_EQ(boundary_triangles.size() % 12U, 0U);
        ASSERT_GE(full_triangles.size(), boundary_triangles.size());
        std::string const zoom_name = zoom < 6.0F ? "5p35" : "6p00";
        ASSERT_TRUE(writeMapCpuCapture(*image, map_layout, boundary_overlay, boundary_triangles,
            WIDTH, HEIGHT, std::string(directory) + "/cpu-zoom-" + zoom_name + "-boundaries.ppm"));
        ASSERT_TRUE(writeMapCpuCapture(*image, map_layout, full_overlay, full_triangles,
            WIDTH, HEIGHT, std::string(directory) + "/cpu-zoom-" + zoom_name + "-with-arrows.ppm"));
        auto const base_view = client::composeS2WorldMapView(&*image, map_layout, boundary_overlay, WIDTH, HEIGHT);
        ASSERT_TRUE(base_view);
        ASSERT_TRUE(renderer.setMapImage(base_view->width, base_view->height, base_view->rgba));
        ASSERT_TRUE(renderer.setGuiVectorTriangles(boundary_triangles));
        renderer.clearGuiText();
        auto const boundary_capture = renderer.render({});
        ASSERT_EQ(boundary_capture.width, WIDTH);
        ASSERT_EQ(boundary_capture.height, HEIGHT);
        ASSERT_EQ(boundary_capture.rgba8.size(), static_cast<size_t>(WIDTH) * HEIGHT * 4U);
        ASSERT_FALSE(boundary_capture.srgb_encoded);
        auto const boundary_written = testsupport::writePpm({
            .width = boundary_capture.width,
            .height = boundary_capture.height,
            .srgb_encoded = boundary_capture.srgb_encoded,
            .pixels = boundary_capture.rgba8,
        }, std::filesystem::path{std::string(directory) + "/vulkan-zoom-" + zoom_name + "-boundaries.ppm"},
            "P16 actual 640x480 Vulkan offscreen, V10 plates, boundaries only");
        ASSERT_TRUE(boundary_written.has_value()) << boundary_written.error();

        ASSERT_TRUE(renderer.setGuiVectorTriangles(full_triangles));
        renderer.clearGuiText();
        for (client::TextLabel const& label : full_text.labels()) {
            bool const plate_identifier = label.value.size() >= 2U && label.value[0U] == 'P'
                && std::all_of(label.value.begin() + 1, label.value.end(), [](char const character) {
                    return character >= '0' && character <= '9';
                });
            if (!plate_identifier) {
                continue;
            }
            renderer.addGuiText(label.value, label.placement, label.color);
        }
        auto const full_capture = renderer.render({});
        ASSERT_EQ(full_capture.width, WIDTH);
        ASSERT_EQ(full_capture.height, HEIGHT);
        ASSERT_EQ(full_capture.rgba8.size(), static_cast<size_t>(WIDTH) * HEIGHT * 4U);
        ASSERT_FALSE(full_capture.srgb_encoded);
        auto const full_written = testsupport::writePpm({
            .width = full_capture.width,
            .height = full_capture.height,
            .srgb_encoded = full_capture.srgb_encoded,
            .pixels = full_capture.rgba8,
        }, std::filesystem::path{std::string(directory) + "/vulkan-zoom-" + zoom_name + "-with-arrows.ppm"},
            "P16 actual 640x480 Vulkan offscreen, V10 plates, with arrows");
        ASSERT_TRUE(full_written.has_value()) << full_written.error();

        size_t boundary_core_pixels = 0U;
        size_t horizontal_boundary_core_pixels = 0U;
        size_t vertical_boundary_core_pixels = 0U;
        // The Vulkan readback is linear; opaque plate cores are approximately (170,95,216).
        auto const is_boundary_core_pixel = [&](uint32_t const x, uint32_t const y) {
            size_t const offset = (static_cast<size_t>(y) * WIDTH + x) * 4U;
            auto const& rgba = boundary_capture.rgba8;
            return rgba[offset] > 150U && rgba[offset + 1U] > 70U && rgba[offset + 1U] < 130U
                && rgba[offset + 2U] > 190U;
        };
        for (uint32_t y = 0U; y < HEIGHT; ++y) {
            for (uint32_t x = 0U; x < WIDTH; ++x) {
                if (!is_boundary_core_pixel(x, y)) continue;
                ++boundary_core_pixels;
                bool const horizontal_neighbor = (x > 0U && is_boundary_core_pixel(x - 1U, y))
                    || (x + 1U < WIDTH && is_boundary_core_pixel(x + 1U, y));
                bool const vertical_neighbor = (y > 0U && is_boundary_core_pixel(x, y - 1U))
                    || (y + 1U < HEIGHT && is_boundary_core_pixel(x, y + 1U));
                horizontal_boundary_core_pixels += horizontal_neighbor ? 1U : 0U;
                vertical_boundary_core_pixels += vertical_neighbor ? 1U : 0U;
            }
        }
        ASSERT_EQ(renderer.validationErrorCount(), 0U);
        manifest << "zoom=" << zoom << " draws=" << map_layout.world_draw_count
            << " source_edges=" << image->plate_boundaries.size()
            << " boundary_clipped_strokes=" << boundary_triangles.size() / 12U
            << " boundary_vertices=" << boundary_triangles.size()
            << " arrows_and_other_vertices=" << full_triangles.size() - boundary_triangles.size()
            << " full_vertices=" << full_triangles.size()
            << " offscreen=" << boundary_capture.width << 'x' << boundary_capture.height
            << " validation_errors=" << renderer.validationErrorCount()
            << " boundary_core_pixels=" << boundary_core_pixels
            << " boundary_core_pixels_with_horizontal_neighbor=" << horizontal_boundary_core_pixels
            << " boundary_core_pixels_with_vertical_neighbor=" << vertical_boundary_core_pixels << '\n';
    }
    ASSERT_TRUE(manifest);
}

TEST(S2MapPresentationTest, DISABLED_CaptureP11WeatherAtBothMapScales)
{
    std::vector<float> u(64U, 1.0F);
    std::vector<float> v(64U, .25F);
    std::vector<float> precipitation(64U, 1.0F);
    client::S2MapWeatherFields const fields{
        .resolution = 8U, .precipitation_resolution = 8U,
        .wind_u = u, .wind_v = v, .precipitation = precipitation, .precipitation_maximum = 1.0F,
    };
    char const* const capture_prefix = std::getenv("MC_WEATHER_CAPTURE_PREFIX");
    for (uint32_t const side : {512U, 1'024U}) {
        client::S2MapLayout layout;
        layout.world_draw_count = 1U;
        layout.world_draws[0U].destination = {0.0F, 0.0F, static_cast<float>(side), static_cast<float>(side)};
        for (bool const rain : {true, false}) {
            for (uint32_t frame = 0U; frame < 24U; ++frame) {
                std::vector<client::GuiVectorVertex> triangles;
                client::appendS2MapWeatherTriangles(triangles, layout, fields, frame / 24.0F,
                    1.0F, !rain, false, rain);
                std::string const filename = std::string{capture_prefix != nullptr ? capture_prefix : "/tmp/mc-p11-weather-"}
                    + std::string(rain ? "rain-" : "wind-")
                    + std::to_string(side) + "-" + std::to_string(frame) + ".ppm";
                ASSERT_TRUE(writeWeatherCpuCapture(triangles, side, filename));
            }
        }
    }
}

TEST(S2MapPresentationTest, NativeIceCoverReplacesGreenTerrainHueWithoutFlatteningHeight)
{
    std::array<float, 2U> const range{-1'000.0F, 3'000.0F};
    auto const bare = client::s2NativeTerrainColor(0.0F, range, {0.0F, 0.0F}, false, 0.0F, 0U);
    EXPECT_EQ(bare, (std::array<float, 3U>{126.0F, 151.0F, 119.0F}));
    auto const snow = client::s2NativeTerrainColor(0.0F, range, {0.0F, 0.0F}, false, 0.0F, 1U);
    auto const glacier = client::s2NativeTerrainColor(0.0F, range, {0.0F, 0.0F}, false, 0.0F, 2U);
    auto const sea_ice = client::s2NativeTerrainColor(0.0F, range, {0.0F, 0.0F}, true, 400.0F, 3U);
    EXPECT_GT(*std::min_element(snow.begin(), snow.end()), 210.0F);
    EXPECT_LT(*std::max_element(snow.begin(), snow.end()) - *std::min_element(snow.begin(), snow.end()), 12.0F);
    EXPECT_GT(*std::min_element(glacier.begin(), glacier.end()), 185.0F);
    EXPECT_GT(*std::min_element(sea_ice.begin(), sea_ice.end()), 180.0F);
    EXPECT_NE(snow, glacier);
    EXPECT_NE(glacier, sea_ice);
    EXPECT_NE(snow, sea_ice);
    auto const summit = client::s2NativeTerrainColor(3'000.0F, range,
        {3'000.0F, 3'000.0F}, false, 0.0F, 1U);
    EXPECT_NE(snow, summit);
    EXPECT_LT(std::abs(snow[0U] - summit[0U]), 24.0F);
    auto const contour = client::s2NativeTerrainColor(0.0F, range, {600.0F, 0.0F}, false, 0.0F, 1U);
    EXPECT_NE(snow, contour);
}

TEST(S2MapPresentationTest, DISABLED_CaptureP13CanonicalPolarMap)
{
    char const* directory = std::getenv("MC_P13_MAP_CAPTURE_DIR");
    ASSERT_NE(directory, nullptr);
    generation::GenerationConfiguration configuration{};
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr);
    shared::TerrainWorld world(atlas, 17U);
    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    std::vector<uint8_t> inland_projection;
    for (bool const heights : {true, false}) {
        client::S2WorldMapState state;
        state.layers.fill(false);
        state.layers[static_cast<size_t>(client::S2WorldMapLayer::Heights)] = heights;
        state.layers[static_cast<size_t>(client::S2WorldMapLayer::Biomes)] = true;
        auto const image = client::rasterizeS2WorldMap(world, *lod, state);
        ASSERT_TRUE(image);
        if (heights) inland_projection = image->v10_projected_inland_ice_kind;
        std::ofstream output(std::string(directory) + (heights ? "/height-biomes.ppm" : "/biomes.ppm"),
            std::ios::binary);
        ASSERT_TRUE(output);
        output << "P6\n" << image->width << ' ' << image->height << "\n255\n";
        for (auto const& pixel : image->rgba) {
            output.write(reinterpret_cast<char const*>(pixel.data()), 3);
        }
        ASSERT_TRUE(output);
    }
    client::S2WorldMapState plate_state;
    plate_state.layers.fill(false);
    plate_state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    plate_state.overview = true;
    auto plates = client::rasterizeS2WorldMap(world, *lod, plate_state);
    ASSERT_TRUE(plates);
    plates->plate_motions.clear();
    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> boundaries;
    static_cast<void>(client::buildS2WorldMapOverlay(plate_state, &*plates, &world,
        32'768.0, 32'768.0, 1'536U, 512U, 1.0F, labels, boundaries));
    ASSERT_TRUE(writeWeatherCpuCapture(boundaries, 1'536U, std::string(directory) + "/plates-wide.ppm"));
    std::ofstream counts(std::string(directory) + "/plate-counts.txt");
    counts << "source_segments=" << plates->plate_boundaries.size() << "\nvertices=" << boundaries.size() << '\n';
    std::ofstream values(std::string(directory) + "/polar-fields.csv");
    ASSERT_TRUE(values);
    values << "x,y,temp,swe,glacier,sea_ice,elevation,water_level,ocean,lake,river,cover\n";
    std::ofstream projection(std::string(directory) + "/inland-ice-projection.csv");
    ASSERT_TRUE(projection);
    projection << "x,y,physical_cover,projected_display_kind\n";
    for (uint32_t y = 0U; y < 64U; ++y) {
        for (uint32_t x = 128U; x < 200U; ++x) {
            auto const sample = generation::sampleV10AnnualMeanSurface(*atlas, x * 256.0, y * 256.0, 0U);
            projection << x << ',' << y << ','
                << static_cast<unsigned>(generation::classifyV10AnnualMeanSurfaceCover(sample)) << ','
                << static_cast<unsigned>(inland_projection[static_cast<size_t>(y) * 256U + x]) << '\n';
            values << x << ',' << y << ',' << sample.temperature_celsius << ',' << sample.snow_water_equivalent_m
                << ',' << sample.glacier_thickness_m << ',' << sample.sea_ice_thickness_m
                << ',' << sample.elevation_m << ',' << sample.water_level_m << ',' << sample.ocean
                << ',' << sample.lake << ',' << sample.river << ','
                << static_cast<unsigned>(generation::classifyV10AnnualMeanSurfaceCover(sample)) << '\n';
        }
    }
    ASSERT_TRUE(values);
}

TEST(S2MapPresentationTest, InlandWaterAboveSeaLevelRetainsWaterHueInsteadOfLandElevationHue)
{
    auto const low = client::s2NativeTerrainColor(10.0F, {-1'000.0F, 3'000.0F},
        {10.0F, 10.0F}, true, 5.0F, 0U);
    auto const high = client::s2NativeTerrainColor(2'000.0F, {-1'000.0F, 3'000.0F},
        {2'000.0F, 2'000.0F}, true, 5.0F, 0U);
    EXPECT_EQ(low, high);
    EXPECT_LT(low[0U], 50.0F);
    EXPECT_LT(low[1U], 90.0F);
    EXPECT_GT(low[2U], low[1U]);
    auto const covered = client::s2NativeTerrainColor(10.0F, {-1'000.0F, 3'000.0F},
        {10.0F, 10.0F}, true, 5.0F, 3U);
    EXPECT_GT(*std::min_element(covered.begin(), covered.end()), 180.0F);
}

TEST(S2MapPresentationTest, PlateForegroundRemainsVisibleAtCornersAndJunctionsInEitherOrder)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    image->plate_motions.clear();
    client::TextRenderer labels;
    auto const contains = [](auto const& a, auto const& b, auto const& c, float x, float y) {
        auto const side = [x, y](auto const& p, auto const& q) {
            return (x - p[0U]) * (q[1U] - p[1U]) - (y - p[1U]) * (q[0U] - p[0U]);
        };
        float const ab = side(a, b), bc = side(b, c), ca = side(c, a);
        return (ab >= 0.0F && bc >= 0.0F && ca >= 0.0F)
            || (ab <= 0.0F && bc <= 0.0F && ca <= 0.0F);
    };
    for (bool const junction : {false, true}) {
        for (bool const reversed : {false, true}) {
            image->plate_boundaries = {
                {.start = {.35F, .5F}, .end = {.5F, .5F}},
                {.start = {.5F, .5F}, .end = {.5F, .65F}},
            };
            if (junction) {
                image->plate_boundaries.push_back({.start = {.5F, .5F}, .end = {.65F, .5F}});
            }
            if (reversed) {
                std::reverse(image->plate_boundaries.begin(), image->plate_boundaries.end());
            }
            std::vector<client::GuiVectorVertex> triangles;
            static_cast<void>(client::buildS2WorldMapOverlay(state, &*image, world.get(),
                32'768.0, 32'768.0, 512U, 512U, 1.0F, labels, triangles));
            for (auto const point : {std::array<float, 2U>{254.0F, 256.5F},
                     std::array<float, 2U>{256.5F, 258.0F},
                     std::array<float, 2U>{256.8F, 255.2F}}) {
                std::array<uint8_t, 4U> top{};
                for (size_t index = 0U; index + 2U < triangles.size(); index += 3U) {
                    if (contains(triangles[index].position, triangles[index + 1U].position,
                            triangles[index + 2U].position, point[0U], point[1U])) {
                        top = triangles[index].rgba;
                    }
                }
                EXPECT_EQ(top, (std::array<uint8_t, 4U>{213U, 164U, 237U, 255U}))
                    << "junction=" << junction << " reversed=" << reversed;
            }
        }
    }
}

TEST(S2MapPresentationTest, ZoomedPlateBorderKeepsVisibleTailShorterThanOneSourceThousandth)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    state.zoom = 6.0;
    // This native grid edge leaves nine screen pixels before the right crop.
    // Its visible UV length is .0008, so a pixel-space cutoff must follow projection.
    state.center_x = (154.0 / 256.0 + .0008 - 1.0 / 12.0) * 65'536.0;
    auto image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    image->plate_motions.clear();
    image->plate_boundaries = {
        {.start = {154.0F / 256.0F, .5F}, .end = {155.0F / 256.0F, .5F}},
    };
    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(state, &*image, world.get(),
        32'768.0, 32'768.0, 1'920U, 1'152U, 1.0F, labels, triangles));
    float left = 1'920.0F;
    float right = 0.0F;
    for (auto const& vertex : triangles) {
        if (vertex.rgba == (std::array<uint8_t, 4U>{213U, 164U, 237U, 255U})) {
            left = std::min(left, vertex.position[0U]);
            right = std::max(right, vertex.position[0U]);
        }
    }
    EXPECT_GT(right - left, 6.0F);
    EXPECT_GT(left, 1'909.0F);
    EXPECT_LE(right, 1'917.01F);
}

TEST(S2MapPresentationTest, PlateMotionArrowHasReadableScreenReachInCompactAndWideViews)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    image->plate_boundaries.clear();
    image->plate_motions = {{.id = 1U, .x = .5F, .y = .5F, .vector_x = 1.0F, .vector_y = 0.0F}};
    for (uint32_t const width : {640U, 1'920U}) {
        client::TextRenderer labels;
        std::vector<client::GuiVectorVertex> triangles;
        static_cast<void>(client::buildS2WorldMapOverlay(state, &*image, world.get(),
            32'768.0, 32'768.0, width, 1'152U, 1.0F, labels, triangles));
        float left = static_cast<float>(width);
        float right = 0.0F;
        for (auto const& vertex : triangles) {
            if (vertex.rgba == (std::array<uint8_t, 4U>{255U, 184U, 64U, 255U})) {
                left = std::min(left, vertex.position[0U]);
                right = std::max(right, vertex.position[0U]);
            }
        }
        EXPECT_GE(right - left, width == 640U ? 64.0F : 72.0F);
    }
}

TEST(S2MapPresentationTest, PlateOverlayKeepsLaterBordersBeyondTheFormerPrivateBudget)
{
    auto const world = mapWorld();
    auto const lod = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    client::S2WorldMapState state;
    state.layers.fill(false);
    state.layers[static_cast<size_t>(client::S2WorldMapLayer::Plates)] = true;
    auto image = client::rasterizeS2WorldMap(*world, *lod, state);
    ASSERT_TRUE(image);
    image->plate_boundaries.clear();
    image->plate_motions.clear();
    for (uint32_t index = 0U; index < 4'000U; ++index) {
        float const x = .1F + static_cast<float>(index % 80U) * .01F;
        float const y = .2F + static_cast<float>(index / 80U) * .008F;
        image->plate_boundaries.push_back({.start = {x, y}, .end = {x + .003F, y}});
    }
    image->plate_boundaries.push_back({.start = {.1F, .8F}, .end = {.9F, .8F}});
    client::TextRenderer labels;
    std::vector<client::GuiVectorVertex> triangles;
    static_cast<void>(client::buildS2WorldMapOverlay(state, &*image, world.get(),
        32'768.0, 32'768.0, 512U, 512U, 1.0F, labels, triangles));
    EXPECT_GT(triangles.size(), 43'200U);
    EXPECT_LE(triangles.size(), client::MAXIMUM_GUI_VECTOR_VERTICES);
    EXPECT_TRUE(std::any_of(triangles.begin(), triangles.end(), [](auto const& vertex) {
        return vertex.rgba == (std::array<uint8_t, 4U>{213U, 164U, 237U, 255U})
            && vertex.position[0U] > 450.0F && std::abs(vertex.position[1U] - 409.6F) < 4.0F;
    }));
}

TEST(S2MapPresentationTest, AnnualInlandIceProjectionUsesColdDominantForcingOnActualWater)
{
    std::array<double, 8U> cold{-10.0, -8.0, -5.0, -2.0, -1.0, -3.0, -6.0, -9.0};
    EXPECT_EQ(client::s2ProjectedInlandIceKind(cold, false, true, false), client::S2_MAP_PROJECTED_INLAND_ICE);
    EXPECT_EQ(client::s2ProjectedInlandIceKind(cold, false, false, true), client::S2_MAP_PROJECTED_INLAND_ICE);
    EXPECT_EQ(client::s2ProjectedInlandIceKind(cold, true, true, false), 0U);
    EXPECT_EQ(client::s2ProjectedInlandIceKind(cold, false, false, false), 0U);
    cold[3U] = 1.0;
    EXPECT_EQ(client::s2ProjectedInlandIceKind(cold, false, true, false), client::S2_MAP_PROJECTED_INLAND_ICE);
    std::array<double, 8U> const warm{-1.0, -1.0, -1.0, 20.0, -1.0, -1.0, -1.0, -1.0};
    EXPECT_EQ(client::s2ProjectedInlandIceKind(warm, false, true, false), 0U);
    std::array<double, 8U> const balanced{-1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0};
    EXPECT_EQ(client::s2ProjectedInlandIceKind(balanced, false, true, false), 0U);
    cold[3U] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(client::s2ProjectedInlandIceKind(cold, false, true, false), 0U);
    auto const projected = client::s2NativeTerrainColor(30.0F, {-1'000.0F, 3'000.0F},
        {30.0F, 30.0F}, true, 5.0F, client::S2_MAP_PROJECTED_INLAND_ICE);
    EXPECT_GT(*std::min_element(projected.begin(), projected.end()), 180.0F);
    EXPECT_LT(*std::max_element(projected.begin(), projected.end())
        - *std::min_element(projected.begin(), projected.end()), 20.0F);
}

namespace {

client::S2MapLayout rainSplitLayout(float const split)
{
    static constexpr float SIDE = 1'536.0F;
    client::S2MapLayout layout;
    layout.world_draw_count = split == 1.0F ? 1U : 2U;
    layout.world_draws[0U].source = {0.0F, 0.0F, split, 1.0F};
    layout.world_draws[0U].destination = {0.0F, 0.0F, SIDE * split, SIDE};
    if (layout.world_draw_count == 2U) {
        layout.world_draws[1U].source = {split, 0.0F, 1.0F - split, 1.0F};
        layout.world_draws[1U].destination = {SIDE * split, 0.0F, SIDE * (1.0F - split), SIDE};
    }
    return layout;
}

std::vector<client::GuiVectorVertex> rainSplitVertices(float const split, bool const weather)
{
    std::vector<float> const precipitation(64U, 1.0F);
    client::S2MapWeatherFields const fields{
        .precipitation_resolution = 8U,
        .precipitation = precipitation,
        .precipitation_maximum = 1.0F,
    };
    std::vector<client::GuiVectorVertex> vertices;
    if (weather) {
        client::appendS2MapWeatherTriangles(vertices, rainSplitLayout(split), fields, .2F,
            1.0F, false, false, true);
    } else {
        client::appendS2MapRainTriangles(vertices, rainSplitLayout(split), fields, .2F);
    }
    return vertices;
}

std::vector<std::array<int32_t, 6U>> rainInteriorVertices(
    std::span<client::GuiVectorVertex const> vertices, float const split)
{
    static constexpr float SIDE = 1'536.0F;
    static constexpr float MARGIN = 12.0F;
    std::vector<std::array<int32_t, 6U>> interior;
    for (size_t first = 0U; first + 2U < vertices.size(); first += 3U) {
        bool outside = false;
        for (size_t index = first; index < first + 3U; ++index) {
            auto const point = vertices[index].position;
            outside = outside || point[0U] < MARGIN || point[0U] > SIDE - MARGIN
                || point[1U] < MARGIN || point[1U] > SIDE - MARGIN
                || std::abs(point[0U] - SIDE * split) < MARGIN;
        }
        if (outside) {
            continue;
        }
        for (size_t index = first; index < first + 3U; ++index) {
            auto const& vertex = vertices[index];
            interior.push_back({static_cast<int32_t>(std::lround(vertex.position[0U] * 128.0F)),
                static_cast<int32_t>(std::lround(vertex.position[1U] * 128.0F)),
                vertex.rgba[0U], vertex.rgba[1U], vertex.rgba[2U], vertex.rgba[3U]});
        }
    }
    std::sort(interior.begin(), interior.end());
    return interior;
}

} // namespace

TEST(S2MapPresentationTest, RainAnchorsSurviveEquivalentViewportSplits)
{
    for (bool const weather : {false, true}) {
        auto const whole = rainSplitVertices(1.0F, weather);
        ASSERT_FALSE(whole.empty());
        for (float const split : {.25F, .5F, .75F}) {
            SCOPED_TRACE(::testing::Message() << "weather=" << weather << " split=" << split);
            auto const expected = rainInteriorVertices(whole, split);
            auto const divided = rainInteriorVertices(rainSplitVertices(split, weather), split);
            ASSERT_GT(expected.size(), 1'000U);
            ASSERT_EQ(expected.size(), divided.size());
            std::multiset<std::array<int32_t, 6U>> unmatched(divided.begin(), divided.end());
            size_t missing_vertices = 0U;
            for (auto const& expected_vertex : expected) {
                auto found = unmatched.end();
                for (int32_t dx = -1; dx <= 1 && found == unmatched.end(); ++dx) {
                    for (int32_t dy = -1; dy <= 1 && found == unmatched.end(); ++dy) {
                        auto neighbor = expected_vertex;
                        neighbor[0U] += dx;
                        neighbor[1U] += dy;
                        found = unmatched.find(neighbor);
                    }
                }
                if (found == unmatched.end()) {
                    ++missing_vertices;
                } else {
                    unmatched.erase(found);
                }
            }
            // Match colors and multiplicity exactly; tolerate at most 1/128-pixel projection rounding.
            EXPECT_EQ(missing_vertices, 0U);
        }
    }
}

TEST(S2MapPresentationTest, DISABLED_CaptureRainWrapViewportSplits)
{
    char const* const directory = std::getenv("MC_RAIN_WRAP_CAPTURE_DIR");
    ASSERT_NE(directory, nullptr);
    client::InstalledShaderAssets const shaders;
    client::VulkanOffscreenRenderer renderer{shaders, true};
    renderer.setDebugHudEnabled(false);
    ASSERT_TRUE(renderer.validationEnabled());
    std::array<std::array<uint8_t, 4U>, 1U> const background{{{65U, 83U, 59U, 255U}}};
    ASSERT_TRUE(renderer.setMapImage(1U, 1U, background));
    for (float const split : {1.0F, .25F, .5F, .75F}) {
        auto vertices = rainSplitVertices(split, true);
        // The existing offscreen target is 640x480; capture the full 1536-square
        // viewport there without changing the production rain construction.
        for (auto& vertex : vertices) {
            vertex.position[0U] *= 640.0F / 1'536.0F;
            vertex.position[1U] *= 480.0F / 1'536.0F;
        }
        ASSERT_TRUE(renderer.setGuiVectorTriangles(vertices));
        auto const capture = renderer.render({});
        ASSERT_EQ(capture.width, 640U);
        ASSERT_EQ(capture.height, 480U);
        ASSERT_EQ(renderer.validationErrorCount(), 0U);
        auto const written = testsupport::writePpm({
            .width = capture.width,
            .height = capture.height,
            .srgb_encoded = capture.srgb_encoded,
            .pixels = capture.rgba8,
        }, std::filesystem::path{directory} / ("split-" + std::to_string(split) + ".ppm"),
            "Actual Vulkan offscreen 640x480, complete 1536-square rain viewport scaled to fit");
        ASSERT_TRUE(written.has_value()) << written.error();
    }
}
