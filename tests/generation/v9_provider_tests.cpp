#include "V9Terrain.hpp"

#include <client/S2MapPresentation.hpp>
#include <client/render/CanonicalTerrainField.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>
#include <generation/V9Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

TEST(V9ProviderTests, BuildsSeededAtlasAndFeedsWorldQueriesAndMapLayers)
{
    constexpr uint32_t SOURCE_RESOLUTION = 256U;
    constexpr size_t SOURCE_COUNT = static_cast<size_t>(SOURCE_RESOLUTION) * SOURCE_RESOLUTION;
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V9;

    std::vector<generation::GenerationStage> stage_order;
    std::vector<std::shared_ptr<generation::GenerationSnapshot const>> snapshots;
    auto const atlas = generation::prepareAtlas(configuration, {}, [&](generation::GenerationProgress const& progress) {
        if (stage_order.empty() || stage_order.back() != progress.stage) {
            stage_order.push_back(progress.stage);
        }
        if (progress.snapshot)
        {
            EXPECT_EQ(progress.snapshot->stage, progress.stage);
            EXPECT_TRUE(progress.snapshot->v9.has_value());
            EXPECT_TRUE(generation::isValidGenerationSnapshot(*progress.snapshot));
            snapshots.push_back(progress.snapshot);
        }
        if (progress.total != 0U) {
            EXPECT_LE(progress.completed, progress.total);
        }
    });

    ASSERT_NE(atlas, nullptr);
    EXPECT_EQ(atlas->configuration, configuration);
    EXPECT_TRUE(generation::isValidAtlas(*atlas));
    EXPECT_TRUE(atlas->elevation.empty());
    EXPECT_TRUE(atlas->temperature.empty());
    EXPECT_EQ(stage_order, (std::vector<generation::GenerationStage>{
        generation::GenerationStage::V9Platec,
        generation::GenerationStage::V9Terrain,
        generation::GenerationStage::V9Climate,
        generation::GenerationStage::V9Water,
        generation::GenerationStage::Complete,
    }));
    ASSERT_EQ(snapshots.size(), 4U);
    EXPECT_EQ(snapshots[0U]->stage, generation::GenerationStage::V9Platec);
    EXPECT_EQ(snapshots[1U]->stage, generation::GenerationStage::V9Terrain);
    EXPECT_EQ(snapshots[2U]->stage, generation::GenerationStage::V9Climate);
    EXPECT_EQ(snapshots[3U]->stage, generation::GenerationStage::V9Water);
    ASSERT_TRUE(snapshots[0U]->v9.has_value());
    EXPECT_FALSE(snapshots[0U]->v9->source_elevation_m.empty());
    auto const preview_source = generation::detail::generateV9PlatecSource(configuration.seed);
    ASSERT_TRUE(preview_source.has_value());
    ASSERT_EQ(snapshots[0U]->v9->source_elevation_m.size(), preview_source->height_raw.size());
    EXPECT_TRUE(std::equal(
        snapshots[0U]->v9->source_elevation_m.begin(),
        snapshots[0U]->v9->source_elevation_m.end(),
        preview_source->height_raw.begin(),
        [](float const source_metres, float const raw_height) {
            return source_metres == generation::detail::v9PlatecHeightToSourceMetres(raw_height);
        }
    ));
    EXPECT_FALSE(snapshots[0U]->v9->plate_ids.empty());
    EXPECT_FALSE(snapshots[0U]->v9->plate_directions.empty());
    EXPECT_GT(snapshots[0U]->v9->plate_directions_observed_after_native_step, 0U);
    EXPECT_LT(snapshots[0U]->v9->plate_directions_observed_after_native_step, 2'400U);
    EXPECT_FALSE(snapshots[2U]->v9->temperature_celsius.empty());
    for (size_t snapshot_index : {2U, 3U}) {
        auto const& weather = *snapshots[snapshot_index]->v9;
        EXPECT_EQ(weather.wind_u.size(), SOURCE_COUNT);
        EXPECT_EQ(weather.wind_v.size(), SOURCE_COUNT);
        EXPECT_EQ(weather.current_u.size(), SOURCE_COUNT);
        EXPECT_EQ(weather.current_v.size(), SOURCE_COUNT);
        EXPECT_TRUE(std::any_of(weather.wind_u.begin(), weather.wind_u.end(), [](float value) { return value != 0.0F; }));
        EXPECT_TRUE(std::any_of(weather.current_u.begin(), weather.current_u.end(), [](float value) { return value != 0.0F; }));
    }
    EXPECT_TRUE(snapshots[2U]->v9->precipitation_mm_year.empty());
    EXPECT_FALSE(snapshots[3U]->v9->precipitation_mm_year.empty());
    EXPECT_FALSE(snapshots[3U]->v9->water_level_m.empty());
    EXPECT_FALSE(snapshots[3U]->v9->ice_surface_kind.empty());

    std::vector<generation::V9RegisteredFieldView> const fields = generation::v9RegisteredFields(*atlas);
    ASSERT_EQ(fields.size(), 95U);
    auto const find_field = [&fields](std::string_view const name) -> generation::V9RegisteredFieldView const* {
        auto const found = std::find_if(fields.begin(), fields.end(), [name](auto const& field) {
            return field.name == name;
        });
        return found == fields.end() ? nullptr : &*found;
    };
    for (auto const& [name, member] : std::array{
        std::pair{"u", &generation::V9GenerationSnapshotFields::wind_u},
        std::pair{"v", &generation::V9GenerationSnapshotFields::wind_v},
        std::pair{"ocean_u", &generation::V9GenerationSnapshotFields::current_u},
        std::pair{"ocean_v", &generation::V9GenerationSnapshotFields::current_v}}) {
        auto const* field = find_field(name);
        ASSERT_NE(field, nullptr);
        for (uint32_t native_index : {0U, 257U, 65'535U}) {
            size_t const coarse_index = static_cast<size_t>((native_index / 256U) / 8U) * 32U
                + (native_index % 256U) / 8U;
            double mean = 0.0;
            std::visit([&](auto values) {
                for (size_t phase = 0U; phase < 8U; ++phase) {
                    mean += static_cast<double>(values[phase * 32U * 32U + coarse_index]) / 8.0;
                }
            }, field->values);
            EXPECT_FLOAT_EQ((snapshots[2U]->v9.value().*member)[native_index], static_cast<float>(mean));
            EXPECT_FLOAT_EQ((snapshots[3U]->v9.value().*member)[native_index], static_cast<float>(mean));
        }
    }
    auto const* elevation = find_field("elevation");
    auto const* temperature = find_field("temperature_C");
    auto const* precipitation = find_field("precipitation_mm_year");
    auto const* biome = find_field("biome_id");
    auto const* ocean = find_field("ocean");
    auto const* lake = find_field("lake");
    auto const* river = find_field("river");
    ASSERT_NE(elevation, nullptr);
    ASSERT_NE(temperature, nullptr);
    ASSERT_NE(precipitation, nullptr);
    ASSERT_NE(biome, nullptr);
    ASSERT_NE(ocean, nullptr);
    ASSERT_NE(lake, nullptr);
    ASSERT_NE(river, nullptr);
    EXPECT_EQ(elevation->type, generation::V9FieldType::Float64);
    EXPECT_EQ(elevation->shape, (std::vector<uint32_t>{SOURCE_RESOLUTION, SOURCE_RESOLUTION}));
    EXPECT_EQ(temperature->type, generation::V9FieldType::Float32);
    EXPECT_EQ(temperature->shape, (std::vector<uint32_t>{8U, SOURCE_RESOLUTION, SOURCE_RESOLUTION}));
    EXPECT_EQ(precipitation->type, generation::V9FieldType::Float32);
    EXPECT_EQ(precipitation->shape, (std::vector<uint32_t>{8U, SOURCE_RESOLUTION, SOURCE_RESOLUTION}));
    EXPECT_EQ(biome->type, generation::V9FieldType::UInt8);
    EXPECT_EQ(biome->shape, (std::vector<uint32_t>{SOURCE_RESOLUTION, SOURCE_RESOLUTION}));

    auto const elevation_values = std::get<std::span<double const>>(elevation->values);
    auto const temperature_values = std::get<std::span<float const>>(temperature->values);
    auto const precipitation_values = std::get<std::span<float const>>(precipitation->values);
    auto const biome_values = std::get<std::span<uint8_t const>>(biome->values);
    auto const ocean_values = std::get<std::span<uint8_t const>>(ocean->values);
    auto const lake_values = std::get<std::span<uint8_t const>>(lake->values);
    auto const river_values = std::get<std::span<uint8_t const>>(river->values);
    ASSERT_EQ(elevation_values.size(), SOURCE_COUNT);
    ASSERT_EQ(temperature_values.size(), SOURCE_COUNT * 8U);
    ASSERT_EQ(precipitation_values.size(), SOURCE_COUNT * 8U);
    ASSERT_EQ(biome_values.size(), SOURCE_COUNT);
    ASSERT_EQ(ocean_values.size(), SOURCE_COUNT);
    ASSERT_EQ(lake_values.size(), SOURCE_COUNT);
    ASSERT_EQ(river_values.size(), SOURCE_COUNT);

    shared::TerrainWorld const world{atlas};
    EXPECT_EQ(world.identity().terrain_version, generation::terrainVersion(generation::TerrainModelTag::V9));
    EXPECT_EQ(world.identity().seed, configuration.seed);
    EXPECT_EQ(world.identity().configuration_identity, generation::configurationIdentity(configuration));

    uint32_t const cell_extent = generation::WORLD_SIZE_BLOCKS / SOURCE_RESOLUTION;

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
                uint32_t const world_x = x * cell_extent;
                uint32_t const world_y = y * cell_extent;
                shared::SurfaceTile const surface = world.sampleSurface({
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
    auto const desert_cell = findExposedCell([&](size_t const index, uint32_t, uint32_t) {
        return biome_values[index] == 5U && elevation_values[index] > 0.0 && !hasWater(
            static_cast<uint32_t>(index % SOURCE_RESOLUTION),
            static_cast<uint32_t>(index / SOURCE_RESOLUTION)
        );
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

    auto const expectSourceMaterialAndL3 = [&](std::array<uint32_t, 2U> const cell,
        shared::Block const expected_material) {
        uint32_t const world_x = cell[0U] * cell_extent;
        uint32_t const world_y = cell[1U] * cell_extent;
        size_t const source_index = static_cast<size_t>(cell[1U]) * SOURCE_RESOLUTION + cell[0U];
        shared::SurfaceTile const surface = world.sampleSurface({
            .x = static_cast<int32_t>(world_x / shared::Chunk::SIDE_LENGTH),
            .y = static_cast<int32_t>(world_y / shared::Chunk::SIDE_LENGTH),
            .z = 0,
        });
        shared::TerrainColumn const* const column = surface.columnAt({.x = 0U, .y = 0U});
        ASSERT_NE(column, nullptr);
        ASSERT_FALSE(column->submerged);
        EXPECT_EQ(column->source_biome_id, biome_values[source_index]);
        EXPECT_DOUBLE_EQ(column->source_l0_elevation_m, elevation_values[source_index]);
        bool const source_coast = hasAdjacentOceanOrLake(cell[0U], cell[1U]);
        EXPECT_EQ(column->source_native_coast_adjacent, source_coast);
        if (expected_material == shared::Block::Snow) {
            EXPECT_LT(column->temperature_celsius, 0.0);
        }
        EXPECT_EQ(column->top_material, expected_material);
        EXPECT_EQ(column->materialAt(column->highest_solid_z, surface.block_sea_level), expected_material);

        auto const lod = shared::buildTerrainLod(
            world,
            shared::TerrainLodLevel::L3,
            world_x,
            world_y,
            shared::Chunk::SIDE_LENGTH
        );
        ASSERT_NE(lod, nullptr);
        shared::TerrainLodSample const sample = shared::sampleTerrainLod(
            *lod, static_cast<double>(world_x), static_cast<double>(world_y)
        );
        EXPECT_EQ(sample.source_biome_id, column->source_biome_id);
        EXPECT_EQ(sample.source_native_coast_adjacent, column->source_native_coast_adjacent);
        EXPECT_EQ(sample.top_material, expected_material);

        shared::ServerHeightTileMessage const height_tile{
            .key = {.x = surface.coordinate.x, .y = surface.coordinate.y},
            .revision = surface.identity.revision,
            .token = 1U,
            .surface = surface,
        };
        auto encoded_height_tile = height_tile;
        for (uint32_t index = 0U; index < encoded_height_tile.heights.size(); ++index) {
            encoded_height_tile.heights[index] = static_cast<uint16_t>(
                encoded_height_tile.surface.columns[index].highest_solid_z + 1U
            );
        }
        auto const decoded_message = shared::decodeMessage(shared::encodeMessage(encoded_height_tile));
        ASSERT_TRUE(decoded_message);
        auto const* const decoded_height_tile = std::get_if<shared::ServerHeightTileMessage>(&*decoded_message);
        ASSERT_NE(decoded_height_tile, nullptr);
        shared::TerrainColumn const* const decoded_column = decoded_height_tile->surface.columnAt({.x = 0U, .y = 0U});
        ASSERT_NE(decoded_column, nullptr);
        EXPECT_EQ(decoded_column->source_biome_id, column->source_biome_id);
        EXPECT_DOUBLE_EQ(decoded_column->source_l0_elevation_m, column->source_l0_elevation_m);
        EXPECT_EQ(decoded_column->source_native_coast_adjacent, column->source_native_coast_adjacent);
        EXPECT_EQ(decoded_column->top_material, expected_material);

        shared::SparseWorld authoritative_world{
            {
                .seed = decoded_height_tile->surface.identity.seed,
                .revision = decoded_height_tile->surface.identity.revision,
            },
            shared::SparseWorldGenerationMode::MaterializedOnly,
        };
        ASSERT_TRUE(authoritative_world.setTerrainIdentity(decoded_height_tile->surface.identity));
        ASSERT_TRUE(authoritative_world.publishAuthoritativeSurface(decoded_height_tile->surface));
        shared::SparseBlockQuery const authoritative_query = authoritative_world.queryBlock({
            .x = static_cast<int64_t>(world_x),
            .y = static_cast<int64_t>(world_y),
            .z = decoded_column->highest_solid_z,
        });
        EXPECT_EQ(authoritative_query.block, std::optional<shared::Block>{expected_material});
    };
    expectSourceMaterialAndL3(*desert_cell, shared::Block::Sand);
    expectSourceMaterialAndL3(*coast_cell, shared::Block::Sand);
    expectSourceMaterialAndL3(*summit_cell, shared::Block::Snow);
    auto const allNearestWaterCellsWet = [&](uint32_t const x, uint32_t const y) {
        uint32_t const next_x = (x + 1U) % SOURCE_RESOLUTION;
        uint32_t const next_y = (y + 1U) % SOURCE_RESOLUTION;
        for (uint32_t const source_y : {y, next_y}) {
            for (uint32_t const source_x : {x, next_x}) {
                size_t const index = static_cast<size_t>(source_y) * SOURCE_RESOLUTION + source_x;
                if (ocean_values[index] == 0U && lake_values[index] == 0U && river_values[index] == 0U) {
                    return false;
                }
            }
        }
        return true;
    };
    std::optional<std::array<uint32_t, 2U>> fully_wet_tile;
    std::optional<std::array<uint32_t, 2U>> tile_with_dry_source_cell;
    std::optional<uint32_t> x_seam_wet_tile_y;
    std::optional<uint32_t> y_seam_wet_tile_x;
    for (uint32_t y = 0U; y < SOURCE_RESOLUTION; ++y) {
        for (uint32_t x = 0U; x < SOURCE_RESOLUTION; ++x) {
            if (allNearestWaterCellsWet(x, y)) {
                if (!fully_wet_tile.has_value()) {
                    fully_wet_tile = std::array<uint32_t, 2U>{x, y};
                }
                if (x == SOURCE_RESOLUTION - 1U && !x_seam_wet_tile_y.has_value()) {
                    x_seam_wet_tile_y = y;
                }
                if (y == SOURCE_RESOLUTION - 1U && !y_seam_wet_tile_x.has_value()) {
                    y_seam_wet_tile_x = x;
                }
            } else if (!tile_with_dry_source_cell.has_value()) {
                tile_with_dry_source_cell = std::array<uint32_t, 2U>{x, y};
            }
        }
    }
    ASSERT_TRUE(fully_wet_tile.has_value());
    ASSERT_TRUE(tile_with_dry_source_cell.has_value());
    EXPECT_TRUE(world.hasWaterPresenceThroughoutAtlasCell(
        (*fully_wet_tile)[0U] * cell_extent,
        (*fully_wet_tile)[1U] * cell_extent,
        cell_extent
    ));
    EXPECT_FALSE(world.hasWaterPresenceThroughoutAtlasCell(
        (*tile_with_dry_source_cell)[0U] * cell_extent,
        (*tile_with_dry_source_cell)[1U] * cell_extent,
        cell_extent
    ));
    ASSERT_TRUE(x_seam_wet_tile_y.has_value());
    ASSERT_TRUE(y_seam_wet_tile_x.has_value());
    EXPECT_TRUE(world.hasWaterPresenceThroughoutAtlasCell(
        (SOURCE_RESOLUTION - 1U) * cell_extent,
        *x_seam_wet_tile_y * cell_extent,
        cell_extent
    ));
    EXPECT_TRUE(world.hasWaterPresenceThroughoutAtlasCell(
        *y_seam_wet_tile_x * cell_extent,
        (SOURCE_RESOLUTION - 1U) * cell_extent,
        cell_extent
    ));

    auto const expectSourceMasksAtTileOffsets = [&](std::array<uint32_t, 2U> const tile) {
        for (uint32_t const offset_y : {0U, cell_extent / 2U}) {
            for (uint32_t const offset_x : {0U, cell_extent / 2U}) {
                uint32_t const world_x = tile[0U] * cell_extent + offset_x;
                uint32_t const world_y = tile[1U] * cell_extent + offset_y;
                uint32_t const source_x = (world_x / cell_extent
                    + static_cast<uint32_t>(world_x % cell_extent >= cell_extent / 2U)) % SOURCE_RESOLUTION;
                uint32_t const source_y = (world_y / cell_extent
                    + static_cast<uint32_t>(world_y % cell_extent >= cell_extent / 2U)) % SOURCE_RESOLUTION;
                size_t const index = static_cast<size_t>(source_y) * SOURCE_RESOLUTION + source_x;
                generation::V9SurfaceSample const sample = generation::sampleV9Surface(
                    *atlas,
                    static_cast<double>(world_x),
                    static_cast<double>(world_y),
                    3U
                );
                EXPECT_EQ(sample.ocean, ocean_values[index] != 0U);
                EXPECT_EQ(sample.lake, lake_values[index] != 0U);
                EXPECT_EQ(sample.river, river_values[index] != 0U);
            }
        }
    };
    expectSourceMasksAtTileOffsets(*fully_wet_tile);
    expectSourceMasksAtTileOffsets(*tile_with_dry_source_cell);
    expectSourceMasksAtTileOffsets({SOURCE_RESOLUTION - 1U, *x_seam_wet_tile_y});
    expectSourceMasksAtTileOffsets({*y_seam_wet_tile_x, SOURCE_RESOLUTION - 1U});

    auto atlas_512 = std::make_shared<generation::GlobalAtlas>(*atlas);
    atlas_512->configuration.resolution = 512U;
    shared::TerrainWorld const world_512{atlas_512};
    uint32_t const half_cell_extent = generation::WORLD_SIZE_BLOCKS / 512U;
    auto const wet_source_cell = std::find_if(ocean_values.begin(), ocean_values.end(), [](uint8_t const value) {
        return value != 0U;
    });
    ASSERT_NE(wet_source_cell, ocean_values.end());
    size_t const wet_source_index = static_cast<size_t>(wet_source_cell - ocean_values.begin());
    EXPECT_TRUE(world_512.hasWaterPresenceThroughoutAtlasCell(
        static_cast<uint32_t>(wet_source_index % SOURCE_RESOLUTION) * cell_extent,
        static_cast<uint32_t>(wet_source_index / SOURCE_RESOLUTION) * cell_extent,
        half_cell_extent
    ));

    shared::SurfaceTile const tile = world.sampleSurface({.x = 0, .y = 0, .z = 0});
    EXPECT_EQ(tile.identity, world.identity());
    EXPECT_EQ(tile.block_sea_level, generation::V9_BLOCK_SEA_LEVEL);
    EXPECT_EQ(tile.max_height, generation::V9_MAX_HEIGHT);
    EXPECT_TRUE(shared::isValidSurfaceTile(tile));

    auto const lod = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0);
    ASSERT_NE(lod, nullptr);
    client::S2WorldMapState state;
    auto const image = client::rasterizeS2WorldMap(world, *lod, state);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->source_identity, world.identity());
    EXPECT_EQ(image->source_model_identity, shared::TERRAIN_LOD_MODEL_V9);
    EXPECT_EQ(image->source_resolution, SOURCE_RESOLUTION);
    EXPECT_EQ(image->vector_field_resolution, 32U);
    EXPECT_EQ(image->v9_plate_ids.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_elevation_m.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_temperature_mean_c.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_precipitation_mean_mm_year.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_biome_ids.size(), SOURCE_COUNT);
    EXPECT_EQ(image->v9_moisture_mean_kg_m2.size(), 32U * 32U);
    EXPECT_EQ(image->v9_wind_u_m_s.size(), 32U * 32U);
    EXPECT_EQ(image->v9_wind_v_m_s.size(), 32U * 32U);
    EXPECT_EQ(image->v9_ocean_u_m_s.size(), 32U * 32U);
    EXPECT_EQ(image->v9_ocean_v_m_s.size(), 32U * 32U);
    EXPECT_EQ(image->v9_ocean_cells.size(), 32U * 32U);
    EXPECT_EQ(image->v9_ice_surface_kind.size(), SOURCE_COUNT);
    for (size_t layer = 0U; layer < image->available_layers.size(); ++layer) {
        EXPECT_TRUE(image->available_layers[layer]) << "map layer " << layer;
    }

    double temperature_mean = 0.0;
    double precipitation_mean = 0.0;
    for (size_t phase = 0U; phase < 8U; ++phase) {
        temperature_mean += temperature_values[phase * SOURCE_COUNT];
        precipitation_mean += precipitation_values[phase * SOURCE_COUNT];
    }
    EXPECT_FLOAT_EQ(image->v9_temperature_mean_c[0U], static_cast<float>(temperature_mean / 8.0));
    EXPECT_FLOAT_EQ(image->v9_precipitation_mean_mm_year[0U], static_cast<float>(precipitation_mean / 8.0));
    EXPECT_DOUBLE_EQ(image->v9_elevation_m[0U], elevation_values[0U]);
    EXPECT_EQ(image->v9_biome_ids[0U], biome_values[0U]);
}
