#include <client/ColumnMapClient.hpp>
#include <client/ColumnMapPresentation.hpp>
#include <client/WorldLoading.hpp>

#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

class RenderableColumnMapClient final : public client::ColumnMapClient {
public:
    using client::ColumnMapClient::ColumnMapClient;

    void composeFrameForTest() { render(); }
    [[nodiscard]] bool gpuPresentationSucceededForTest() const { return presentationSucceeded(); }
};

TEST(ColumnMapClientTest, TerrainV2PaletteMatchesFrozenReferencePixels)
{
    // The exact IDs and RGBA8 values are frozen in the Terrain-v2 fixture manifest.
    static constexpr std::array<client::ColumnMapRgba8, 7> EXPECTED_COLORS{
        client::ColumnMapRgba8{ 189U, 217U, 237U, 255U },
        client::ColumnMapRgba8{ 128U, 128U, 128U, 255U },
        client::ColumnMapRgba8{ 92U, 147U, 69U, 255U },
        client::ColumnMapRgba8{ 136U, 103U, 72U, 255U },
        client::ColumnMapRgba8{ 42U, 40U, 49U, 255U },
        client::ColumnMapRgba8{ 244U, 247U, 250U, 255U },
        client::ColumnMapRgba8{ 128U, 201U, 214U, 255U },
    };

    client::ColumnMapPalette const palette = client::canonicalColumnMapPalette();

    for (uint32_t material = 0U; material < EXPECTED_COLORS.size(); ++material) {
        EXPECT_EQ(palette.colors[material], EXPECTED_COLORS[material]) << "material=" << material;
    }
}

TEST(ColumnMapClientTest, DoesNotInventReadyPixelsWithoutAValidatedWorld)
{
    static constexpr client::WorldWindow WINDOW{
        .origin_x = 16,
        .origin_y = 16,
        .width = 2U,
        .height = 2U,
    };
    RenderableColumnMapClient game_client{
        WINDOW,
        client::canonicalColumnMapPalette(),
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        false,
    };

    game_client.composeFrameForTest();

    EXPECT_EQ(game_client.frameState().sequence, 1U);
    EXPECT_FALSE(game_client.frameState().authoritative_world_available);
    EXPECT_TRUE(game_client.frameState().logical_frame_composed);
    EXPECT_EQ(game_client.frameState().map_update.scanned_columns, 0U);
    EXPECT_EQ(game_client.frameState().loading.state, client::WorldLoadingState::Idle);
    EXPECT_FALSE(game_client.frameState().active_coverage_epoch.has_value());
    EXPECT_FALSE(game_client.frameState().active_coverage_center.has_value());
    EXPECT_FALSE(game_client.frameState().terrain_identity.has_value());
    EXPECT_EQ(game_client.frameState().active_materialized_column_presence.size(), 4U);
    for (uint8_t const covered : game_client.frameState().active_materialized_column_presence) {
        EXPECT_EQ(covered, 0U);
    }
    EXPECT_FALSE(game_client.gpuPresentationSucceededForTest());
    EXPECT_EQ(game_client.presentation().window(), WINDOW);
    EXPECT_EQ(game_client.presentation().planes().coverage.size(), 4U);
    for (client::ColumnCoverage const coverage : game_client.presentation().planes().coverage) {
        EXPECT_EQ(coverage, client::ColumnCoverage::Unknown);
    }
}

TEST(ColumnMapClientTest, BoundsWindowAndPixelsToTheSupportedTileSize)
{
    static constexpr client::WorldWindow WINDOW{
        .origin_x = -1,
        .origin_y = 65'535,
        .width = client::ColumnMapPresentation::MAX_TILE_SIDE,
        .height = client::ColumnMapPresentation::MAX_TILE_SIDE,
    };
    static constexpr client::WorldWindow INVALID_WINDOW{
        .origin_x = 0,
        .origin_y = 0,
        .width = client::ColumnMapPresentation::MAX_TILE_SIDE + 1U,
        .height = 1U,
    };
    client::ColumnMapClient game_client{
        WINDOW,
        client::canonicalColumnMapPalette(),
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        false,
    };

    uint32_t const pixel_count = static_cast<uint32_t>(game_client.presentation().planes().coverage.size());
    EXPECT_EQ(pixel_count, 65'536U);
    EXPECT_EQ(game_client.presentation().planes().rgba.size(), pixel_count);
    EXPECT_EQ(game_client.presentation().planes().material_ids.size(), pixel_count);
    EXPECT_EQ(game_client.presentation().planes().highest_solid_z.size(), pixel_count);
    EXPECT_EQ(game_client.frameState().active_materialized_column_presence.size(), pixel_count);
    EXPECT_FALSE(game_client.setWindow(INVALID_WINDOW));
    EXPECT_EQ(game_client.presentation().window(), WINDOW);
    EXPECT_EQ(game_client.presentation().planes().coverage.size(), pixel_count);
}

} // namespace
