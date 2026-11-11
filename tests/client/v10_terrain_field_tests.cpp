#include <client/render/CanonicalTerrainField.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/V10Provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

TEST(V10TerrainFieldTests, RejectsLegacyAtlasInsteadOfReinterpretingItsFields)
{
    generation::GlobalAtlas atlas;
    for (auto const model : {generation::TerrainModelTag::R6, generation::TerrainModelTag::V9,
             generation::TerrainModelTag::V21}) {
        atlas.configuration.model_tag = model;
        EXPECT_THROW((void)client::render::buildV10CanonicalTerrainFieldValues(atlas), std::invalid_argument);
    }
}

TEST(V10TerrainFieldTests, ActualSourceMountainAndMaterialsMatchCanonicalWorld)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    ASSERT_TRUE(atlas);
    EXPECT_NE(generation::configurationIdentity(configuration).find(
        "f775213330028b165328885c6899ac0bf9c3c0c9a93ff6e0aca591554dec6e59"), std::string::npos);
    auto const values = client::render::buildV10CanonicalTerrainFieldValues(*atlas);
    ASSERT_EQ(values.size(), 256U * 256U);
    auto const& mountain = values[39U * 256U + 181U];
    EXPECT_NEAR(mountain[1], 723.342'269'667'154'6, 0.0001);
    shared::TerrainWorld const world{atlas};
    EXPECT_EQ(generation::mapV10ElevationToBlock(mountain[1]), world.sampleSurfaceHeight(46'336U, 9'984U));
    EXPECT_EQ(world.sampleSurfaceHeight(46'336U, 9'984U), 1'235U);
    EXPECT_EQ(world.sampleV10TopMaterial(44'544U, 256U), shared::Block::Sand);
    EXPECT_EQ(values[256U + 174U][2], static_cast<float>(shared::Block::Sand));

    // Observe one real source witness per material, plus the periodic corners.
    std::array<bool, 256U> observed{};
    size_t water_count = 0U;
    for (uint32_t y = 0U; y < 256U; ++y) {
        for (uint32_t x = 0U; x < 256U; ++x) {
            auto const& value = values[static_cast<size_t>(y) * 256U + x];
            auto const source = generation::sampleV10AnnualMeanSurface(*atlas, x * 256.0, y * 256.0, 0U);
            EXPECT_EQ(value[0], static_cast<float>(source.temperature_celsius));
            EXPECT_EQ(value[1], static_cast<float>(source.elevation_m));
            EXPECT_TRUE(std::isfinite(value[0]));
            EXPECT_TRUE(std::isfinite(value[1]));
            if (source.ocean || source.lake || source.river) {
                ++water_count;
            }
            auto const material = static_cast<uint8_t>(value[2]);
            if (!observed[material] || (x == 255U && y == 255U) || (x == 0U && y == 0U)) {
                observed[material] = true;
                auto const tile = world.sampleSurface({
                    .x = static_cast<int32_t>(x * 16U), .y = static_cast<int32_t>(y * 16U), .z = 0});
                auto const& column = tile.columns[0U];
                EXPECT_EQ(material,
                    static_cast<uint8_t>(column.materialAt(column.highest_solid_z, atlas->block_sea_level)))
                    << x << ',' << y;
                EXPECT_EQ(column.highest_solid_z, generation::mapV10ElevationToBlock(source.elevation_m));
                auto const wrapped = generation::sampleV10AnnualMeanSurface(
                    *atlas, x * 256.0 + 65'536.0, y * 256.0 - 65'536.0, 3U);
                EXPECT_DOUBLE_EQ(wrapped.elevation_m, source.elevation_m);
            }
        }
    }
    EXPECT_GT(water_count, 0U);
    EXPECT_LT(water_count, values.size());
    EXPECT_TRUE(observed[static_cast<uint8_t>(shared::Block::Dirt)]);
    EXPECT_TRUE(observed[static_cast<uint8_t>(shared::Block::Snow)]);
}

} // namespace
