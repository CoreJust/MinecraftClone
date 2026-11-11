#include <client/render/DebugHud.hpp>
#include <client/render/GuiRenderer.hpp>
#include <client/render/TextRenderer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

size_t writeMarker(
    char* const destination,
    size_t const capacity,
    double,
    int,
    void*
) noexcept {
    if (capacity == 0U) {
        return 0U;
    }
    destination[0] = 'X';
    return 1U;
}

} // namespace

TEST(DebugHudTest, FormatsCompactTelemetryAndPreservesCameraHelpWithoutViewReadout)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.updateAt(0.0, {
        .player_x = 1.25F,
        .player_y = -2.30F,
        .player_z = 4.56F,
        .camera_yaw_degrees = 45.0F,
        .camera_pitch_degrees = -10.0F,
        .camera_roll_degrees = 3.0F,
        .loaded_dense_chunks_available = true,
        .loaded_dense_chunks = 0U,
        .client_memory_rss_bytes = 1'572'864U,
    });

    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    EXPECT_EQ(
        std::string_view(text.value),
        "FPS:0.0 UPTIME:0.0s\nSPEED:1x (5x selected)\nCAMERA: F5 or 5\n"
        "FLIGHT: OFF BYPASS: OFF (F6)\nXYZ: 1.2 4.6 -2.3\nYPR deg: 45.0 -10.0 3.0\n"
        "DENSE CHUNKS: 0\nCLIENT MEMORY (RSS): 1.5 MiB  CLIMATE: --"
    );
    EXPECT_EQ(text.value.find("VIEW:"), std::string::npos);
    ASSERT_EQ(text.spans.size(), 13U);
    uint32_t const white = client::TextColor{}.packed();
    uint32_t const cyan = client::TextColor{ 0.48F, 0.88F, 1.0F, 1.0F }.packed();
    uint32_t const gold = client::TextColor{ 1.0F, 0.82F, 0.38F, 1.0F }.packed();
    uint32_t const green = client::TextColor{ 0.63F, 1.0F, 0.62F, 1.0F }.packed();
    uint32_t const rose = client::TextColor{ 1.0F, 0.65F, 0.74F, 1.0F }.packed();
    struct ExpectedSpan final {
        size_t index;
        size_t offset;
        size_t length;
        uint32_t color;
    };
    auto expectSpan = [&](ExpectedSpan const expected) {
        ASSERT_LT(expected.index, text.spans.size());
        EXPECT_EQ(text.spans[expected.index].offset, expected.offset);
        EXPECT_EQ(text.spans[expected.index].length, expected.length);
        EXPECT_EQ(text.spans[expected.index].packed_color, expected.color);
    };

    expectSpan({ .index = 0U, .offset = 0U, .length = text.value.find("1.2"), .color = white });
    EXPECT_EQ(text.spans[1].packed_color, cyan);
    EXPECT_EQ(text.spans[3].packed_color, gold);
    EXPECT_EQ(text.spans[5].packed_color, green);
    EXPECT_EQ(text.spans[7].packed_color, cyan);
    EXPECT_EQ(text.spans[9].packed_color, gold);
    EXPECT_EQ(text.spans[11].packed_color, rose);
    EXPECT_EQ(text.spans[12].packed_color, white);
}

TEST(DebugHudTest, UsesTouchHelpAndAccelerationState)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.updateAt(0.0, {
        .touch_flight_help = true,
        .speedup = 30U,
        .selected_speedup = 30U,
        .acceleration_enabled = true,
    });

    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    std::string_view const formatted(text.value);
    EXPECT_NE(formatted.find("TOUCH: UP/DOWN"), std::string_view::npos);
    EXPECT_NE(formatted.find("SPEED:30x (acceleration ON)"), std::string_view::npos);
}

TEST(DebugHudTest, HidesCameraViewAndPreservesEffectiveMovementRights)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.updateAt(0.0, {
        .view_index = 2U,
        .flight_enabled = true,
        .collision_bypass_enabled = true,
    });

    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    std::string_view const formatted(text.value);
    EXPECT_NE(formatted.find("CAMERA: F5 or 5"), std::string_view::npos);
    EXPECT_EQ(formatted.find("VIEW:"), std::string_view::npos);
    EXPECT_NE(formatted.find("FLIGHT: ON BYPASS: ON (F6)"), std::string_view::npos);
    EXPECT_EQ(formatted.find("SURVIVAL"), std::string_view::npos);
    EXPECT_EQ(formatted.find("CREATIVE"), std::string_view::npos);
    EXPECT_EQ(formatted.find("SPECTATOR"), std::string_view::npos);
}

TEST(DebugHudTest, BuildsDynamicGlyphBatch)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    client::DebugHudText text;
    client::DebugHudBatch batch;

    ASSERT_TRUE(hud.buildBatch(text, batch));
    EXPECT_EQ(batch.space, client::TextSpace::Screen);
    EXPECT_EQ(batch.glyphs.size(), text.value.size() - (client::DEBUG_HUD_LINE_COUNT - 1U));
    EXPECT_EQ(batch.glyphs.front().character, static_cast<uint32_t>('F'));
    EXPECT_EQ(batch.glyphs.front().packed_color, hud.lineColors()[0].packed());
}

TEST(DebugHudTest, F1ToggleLatchDebouncesPressesAndCanBeReset)
{
    client::DebugHudToggleLatch latch;
    EXPECT_TRUE(latch.update(true));
    EXPECT_FALSE(latch.update(true));
    EXPECT_FALSE(latch.update(false));
    EXPECT_TRUE(latch.update(true));
    latch.reset();
    EXPECT_TRUE(latch.update(true));
}

TEST(DebugHudTest, CountsUniqueFenceCompletedIdentitiesInTheSlidingWindow)
{
    client::DebugHudState hud;
    client::DebugHudInput input{
        .completed_frame = client::RendererFrameIdentity{ 1U, 4U },
        .completed_frame_observed_at = std::chrono::steady_clock::time_point{},
    };

    hud.updateAt(0.0, input);
    hud.updateAt(0.5, input);
    input.completed_frame = client::RendererFrameIdentity{ 2U, 4U };
    input.completed_frame_observed_at = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{ 500 };
    hud.updateAt(0.5, input);
    input.completed_frame = client::RendererFrameIdentity{ 3U, 4U };
    input.completed_frame_observed_at = std::chrono::steady_clock::time_point{} + std::chrono::seconds{ 1 };
    hud.updateAt(1.0, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 2.0);

    hud.updateAt(2.1, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
}

TEST(DebugHudTest, SwapchainGenerationMakesOtherwiseEqualFrameIdentityFresh)
{
    client::DebugHudState hud;
    client::DebugHudInput input{
        .completed_frame = client::RendererFrameIdentity{ 9U, 2U },
        .completed_frame_observed_at = std::chrono::steady_clock::time_point{},
    };

    hud.updateAt(0.0, input);
    input.completed_frame = client::RendererFrameIdentity{ 9U, 3U };
    input.completed_frame_observed_at = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{ 500 };
    hud.updateAt(0.5, input);

    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 2.0);
}

TEST(DebugHudTest, ClocksNeverRewindAndIgnoreNonFiniteValues)
{
    client::DebugHudState hud;
    hud.updateAt(4.0, {});
    hud.updateAt(2.0, {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 0.0);
    hud.updateAt(5.5, {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 1.5);
    hud.updateAt(std::numeric_limits<double>::infinity(), {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 1.5);
    hud.updateAt(std::numeric_limits<double>::quiet_NaN(), {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 1.5);
}

TEST(DebugHudTest, SupportsInjectedFormattingAndDpiToggleState)
{
    client::DebugHudState hud({}, {
        .format = &writeMarker,
    });
    hud.setEnabled(true);
    hud.setDpiScale(100.0F);
    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    EXPECT_NE(std::string_view(text.value).find("FPS:X"), std::string_view::npos);
    EXPECT_FLOAT_EQ(hud.snapshot().dpi_scale, 8.0F);
    hud.toggle();
    EXPECT_FALSE(hud.formatText(text));
}

TEST(DebugHudTest, SupportsLineColorChanges)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.setLineColor(0U, client::DebugHudColor::Rose);
    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    EXPECT_EQ(text.spans.front().packed_color, hud.lineColors()[0].packed());
}

TEST(DebugHudTest, MeasuresFenceObservationSpacingInsteadOfDelayedHudUpdates)
{
    client::DebugHudState hud;
    auto const epoch = std::chrono::steady_clock::time_point{};
    client::DebugHudInput input{
        .completed_frame = client::RendererFrameIdentity{ 1U, 5U },
        .completed_frame_observed_at = epoch + std::chrono::seconds{ 10 },
    };
    hud.updateAt(10.7, input);
    input.completed_frame = client::RendererFrameIdentity{ 2U, 5U };
    input.completed_frame_observed_at = epoch + std::chrono::milliseconds{ 10'250 };
    hud.updateAt(10.8, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 4.0);
    hud.updateAt(11.3, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
}

TEST(DebugHudTest, DoesNotCreditCompletionsWithoutFenceTimesOrExpiredObservations)
{
    client::DebugHudState hud;
    client::DebugHudInput input{ .completed_frame = client::RendererFrameIdentity{ 1U, 5U } };
    hud.updateAt(10.0, input);
    input.completed_frame = client::RendererFrameIdentity{ 2U, 5U };
    hud.updateAt(10.5, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
    input.completed_frame = client::RendererFrameIdentity{ 3U, 5U };
    input.completed_frame_observed_at = std::chrono::steady_clock::time_point{};
    hud.updateAt(10.6, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
}

TEST(DebugHudTest, MatchesPrototypeMapAndColumnGeometryAcrossViewportAndDpi)
{
    client::DebugHudLayout const desktop = client::layoutDebugHud(1280U, 676U, 1.0F);
    EXPECT_FALSE(desktop.compact);
    EXPECT_FLOAT_EQ(desktop.margin_pixels, 24.0F);
    EXPECT_FLOAT_EQ(desktop.left_column_x, 24.0F);
    EXPECT_FLOAT_EQ(desktop.right_column_x, 534.0F);
    EXPECT_FLOAT_EQ(desktop.column_width_pixels, 470.0F);
    EXPECT_FLOAT_EQ(desktop.minimap.x, 1054.0F);
    EXPECT_FLOAT_EQ(desktop.minimap.y, 44.0F);
    EXPECT_FLOAT_EQ(desktop.minimap.width, 192.0F);
    EXPECT_FLOAT_EQ(desktop.minimap.height, 192.0F);
    EXPECT_FLOAT_EQ(desktop.minimap_interactive.x, 1028.0F);
    EXPECT_FLOAT_EQ(desktop.minimap_interactive.y, 6.0F);
    EXPECT_FLOAT_EQ(desktop.minimap_interactive.width, 244.0F);
    EXPECT_FLOAT_EQ(desktop.minimap_interactive.height, 268.0F);
    EXPECT_FLOAT_EQ(desktop.north.x, 1142.0F);
    EXPECT_FLOAT_EQ(desktop.north.y, 8.0F);
    EXPECT_FLOAT_EQ(desktop.east.x, 1254.0F);
    EXPECT_FLOAT_EQ(desktop.east.y, 124.0F);
    EXPECT_FLOAT_EQ(desktop.south.x, 1142.0F);
    EXPECT_FLOAT_EQ(desktop.south.y, 240.0F);
    EXPECT_FLOAT_EQ(desktop.west.x, 1030.0F);
    EXPECT_FLOAT_EQ(desktop.west.y, 124.0F);
    EXPECT_FLOAT_EQ(desktop.heading.x, 1198.0F);
    EXPECT_FLOAT_EQ(desktop.heading.y, 16.0F);
    EXPECT_FLOAT_EQ(desktop.selected_block_y, 548.0F);
    EXPECT_FLOAT_EQ(desktop.controls_y, 623.0F);

    client::DebugHudLayout const windowed = client::layoutDebugHud(1280U, 676U, 1.0F, true);
    EXPECT_TRUE(windowed.compact);
    EXPECT_FLOAT_EQ(windowed.controls_y, 631.0F);
    // Compact placement preserves full-size text when the logical canvas is roomy.
    EXPECT_FLOAT_EQ(windowed.glyph_scale, 1.0F);
    EXPECT_FLOAT_EQ(windowed.minimap.x, desktop.minimap.x);
    EXPECT_FLOAT_EQ(windowed.minimap.y, desktop.minimap.y);
    EXPECT_FLOAT_EQ(windowed.minimap.width, desktop.minimap.width);
    EXPECT_FLOAT_EQ(windowed.minimap.height, desktop.minimap.height);

    client::DebugHudLayout const compact = client::layoutDebugHud(844U, 346U, 1.0F);
    EXPECT_TRUE(compact.compact);
    EXPECT_FLOAT_EQ(compact.margin_pixels, 12.0F);
    EXPECT_FLOAT_EQ(compact.controls_y, 301.0F);
    EXPECT_FLOAT_EQ(compact.glyph_scale, 0.75F);
    EXPECT_FLOAT_EQ(compact.minimap.x, 682.0F);
    EXPECT_FLOAT_EQ(compact.minimap.y, 44.0F);
    EXPECT_FLOAT_EQ(compact.minimap.width, 128.0F);
    EXPECT_FLOAT_EQ(compact.minimap.height, 128.0F);
    EXPECT_FLOAT_EQ(compact.north.x, 738.0F);
    EXPECT_FLOAT_EQ(compact.north.y, 8.0F);
    EXPECT_FLOAT_EQ(compact.east.x, 818.0F);
    EXPECT_FLOAT_EQ(compact.east.y, 92.0F);
    EXPECT_FLOAT_EQ(compact.south.x, 738.0F);
    EXPECT_FLOAT_EQ(compact.south.y, 176.0F);
    EXPECT_FLOAT_EQ(compact.west.x, 658.0F);
    EXPECT_FLOAT_EQ(compact.west.y, 92.0F);
    EXPECT_FLOAT_EQ(compact.heading.x, 762.0F);
    EXPECT_FLOAT_EQ(compact.heading.y, 16.0F);
    EXPECT_FLOAT_EQ(compact.right_column_x, 338.0F);
    EXPECT_FLOAT_EQ(compact.column_width_pixels, 302.0F);

    client::DebugHudLayout const hidpi = client::layoutDebugHud(2560U, 1352U, 2.0F);
    EXPECT_EQ(hidpi.compact, desktop.compact);
    EXPECT_FLOAT_EQ(hidpi.margin_pixels, desktop.margin_pixels * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.left_column_x, desktop.left_column_x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.right_column_x, desktop.right_column_x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.minimap.x, desktop.minimap.x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.minimap.y, desktop.minimap.y * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.minimap.width, desktop.minimap.width * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.glyph_scale, desktop.glyph_scale * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.north.x, desktop.north.x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.north.y, desktop.north.y * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.east.x, desktop.east.x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.east.y, desktop.east.y * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.south.x, desktop.south.x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.south.y, desktop.south.y * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.west.x, desktop.west.x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.west.y, desktop.west.y * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.heading.x, desktop.heading.x * 2.0F);
    EXPECT_FLOAT_EQ(hidpi.heading.y, desktop.heading.y * 2.0F);
}

TEST(DebugHudTest, BuildsPrototypeReadoutGroupsFromAvailableRuntimeValues)
{
    client::DebugHudSnapshot const snapshot{
        .uptime_seconds = 134.0,
        .presented_fps = 60.0,
        .input = {
            .player_x = 37'888.0F,
            .player_y = 28'672.0F,
            .player_z = 84.1F,
            .camera_yaw_degrees = 180.0F,
            .camera_pitch_degrees = 0.0F,
            .camera_roll_degrees = 0.0F,
            .speedup = 1U,
            .selected_speedup = 5U,
            .loaded_dense_chunks_available = true,
            .loaded_dense_chunks = 18U,
            .client_memory_rss_bytes = 44'040'192U,
            .temperature_celsius = 16.6F,
        },
        .dpi_scale = 1.0F,
        .enabled = true,
    };
    client::TextRenderer labels;
    client::DebugHudLayout const layout = client::layoutDebugHud(1280U, 676U, 1.0F);
    client::buildDebugHudPresentation(snapshot, layout, labels);

    auto const hasLabel = [&labels](std::string_view const value) {
        return std::any_of(labels.labels().begin(), labels.labels().end(), [value](auto const& label) {
            return label.value == value;
        });
    };
    EXPECT_TRUE(hasLabel("FPS 60"));
    EXPECT_TRUE(hasLabel("Uptime 02:14"));
    EXPECT_TRUE(hasLabel("Cache 18"));
    EXPECT_TRUE(hasLabel("Memory 42 MiB"));
    EXPECT_TRUE(hasLabel("Position"));
    EXPECT_TRUE(hasLabel("Movement"));
    EXPECT_TRUE(hasLabel("Local climate"));
    EXPECT_TRUE(hasLabel("Temperature 16.6 C"));
    EXPECT_TRUE(std::none_of(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
        return label.value.starts_with("Humidity") && label.value.find("Unavailable") != std::string::npos;
    }));
    EXPECT_TRUE(std::none_of(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
        return label.value.find("Selected block") != std::string::npos
            || label.value.starts_with("Mode:")
            || label.value.find("Break") != std::string::npos
            || label.value.find("Place") != std::string::npos;
    }));
    EXPECT_TRUE(hasLabel("F5 View"));
    EXPECT_TRUE(std::none_of(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
        return label.value.find("View:") != std::string::npos;
    }));
    EXPECT_TRUE(std::none_of(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
        return label.value == "N" || label.value == "E" || label.value == "S" || label.value == "W"
            || label.value.ends_with("deg");
    }));
}

TEST(DebugHudTest, DoesNotEmitDuplicateTargetMaterialEntry)
{
    client::DebugHudSnapshot const snapshot{
        .input = {
            .target_block = shared::Block::Ice,
        },
        .enabled = true,
    };
    client::TextRenderer labels;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(1280U, 676U, 1.0F), labels);

    EXPECT_TRUE(std::none_of(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
        return label.value.starts_with("Target block ");
    }));
}

TEST(DebugHudTest, ShowsSpacedChunkTelemetryAndCurrentControls)
{
    client::DebugHudSnapshot const snapshot{
        .input = {
            .loaded_dense_chunks_available = true,
            .loaded_dense_chunks = 1024U,
            .current_interest_chunks_available = true,
            .current_interest_resident_chunks = 0U,
            .current_interest_required_chunks = 0U,
        },
        .enabled = true,
    };
    client::TextRenderer labels;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(1280U, 676U, 1.0F), labels);

    auto const hasLabel = [&labels](std::string_view const value) {
        return std::any_of(labels.labels().begin(), labels.labels().end(), [value](auto const& label) {
            return label.value == value;
        });
    };
    EXPECT_TRUE(hasLabel("Chunks 0 | Cache 1024"));
    EXPECT_TRUE(hasLabel("G Mode"));
    EXPECT_FALSE(hasLabel("F6/6 Movement"));
}

TEST(DebugHudTest, PrototypeReadoutsRemainInsideDesktopCompactAndHiDpiCanvases)
{
    client::DebugHudSnapshot const snapshot{
        .uptime_seconds = 86'400.0,
        .presented_fps = 60.0,
        .input = {
            .player_x = 37'888.0F,
            .player_y = 28'672.0F,
            .player_z = 84.1F,
            .camera_yaw_degrees = 180.0F,
            .camera_pitch_degrees = -42.0F,
            .camera_roll_degrees = 0.0F,
            .speedup = 1U,
            .selected_speedup = 99U,
            .acceleration_enabled = true,
            .flight_enabled = true,
            .collision_bypass_enabled = true,
            .loaded_dense_chunks_available = true,
            .loaded_dense_chunks = 513U,
            .client_memory_rss_bytes = 44'040'192U,
            .temperature_celsius = 16.6F,
            .target_block = shared::Block::Bedrock,
        },
        .enabled = true,
    };
    std::array<std::array<uint32_t, 3U>, 5U> const viewports{{
        { 1280U, 676U, 1U },
        { 844U, 346U, 1U },
        { 640U, 360U, 1U },
        { 400U, 600U, 1U },
        { 2560U, 1352U, 2U },
    }};

    for (auto const& viewport : viewports) {
        uint32_t const width = viewport[0U];
        uint32_t const height = viewport[1U];
        float const dpi_scale = static_cast<float>(viewport[2U]);
        client::DebugHudLayout const layout = client::layoutDebugHud(width, height, dpi_scale);
        client::TextRenderer labels;
        client::buildDebugHudPresentation(snapshot, layout, labels);

        EXPECT_TRUE(std::any_of(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
            return label.value == "Position";
        }));
        EXPECT_TRUE(std::any_of(labels.labels().begin(), labels.labels().end(), [&layout](auto const& label) {
            return label.value == (layout.compact
                ? "Temp 16.6 C"
                : "Local climate");
        }));
        for (auto const& label : labels.labels()) {
            float const text_width = static_cast<float>(label.value.size()) * 16.0F * label.placement.scale;
            float const text_height = 32.0F * label.placement.scale;
            EXPECT_GE(label.placement.position[0U], 0.0F) << label.value;
            EXPECT_GE(label.placement.position[1U], 0.0F) << label.value;
            EXPECT_LE(label.placement.position[0U] + text_width, static_cast<float>(width)) << label.value;
            EXPECT_LE(label.placement.position[1U] + text_height, static_cast<float>(height)) << label.value;
        }
    }
}

TEST(DebugHudTest, FullResolutionCompactLayoutPreservesReadableBodyText)
{
    std::array<std::array<uint32_t, 2U>, 2U> const viewports{{
        { 1920U, 1080U },
        { 960U, 540U },
    }};
    client::DebugHudSnapshot const snapshot{
        .presented_fps = 60.0,
        .input = {
            .player_x = 37'888.0F,
            .player_y = 28'672.0F,
            .player_z = 84.1F,
        },
        .enabled = true,
    };
    std::array<float, 2U> glyph_heights{};

    for (size_t index = 0U; index < viewports.size(); ++index) {
        uint32_t const width = viewports[index][0U];
        uint32_t const height = viewports[index][1U];
        client::DebugHudLayout const layout = client::layoutDebugHud(width, height, 1.0F, true);
        ASSERT_TRUE(layout.compact);

        client::TextRenderer labels;
        client::buildDebugHudPresentation(snapshot, layout, labels);
        auto const fps = std::find_if(labels.labels().begin(), labels.labels().end(), [](auto const& label) {
            return label.value == "FPS 60";
        });
        ASSERT_NE(fps, labels.labels().end());
        glyph_heights[index] = client::DEBUG_HUD_GLYPH_HEIGHT_PIXELS * fps->placement.scale;

        for (client::TextLabel const& label : labels.labels()) {
            float const text_width = static_cast<float>(label.value.size())
                * client::DEBUG_HUD_GLYPH_WIDTH_PIXELS * label.placement.scale;
            float const text_height = client::DEBUG_HUD_GLYPH_HEIGHT_PIXELS * label.placement.scale;
            EXPECT_GE(label.placement.position[0U], 0.0F) << label.value;
            EXPECT_GE(label.placement.position[1U], 0.0F) << label.value;
            EXPECT_LE(label.placement.position[0U] + text_width, static_cast<float>(width)) << label.value;
            EXPECT_LE(label.placement.position[1U] + text_height, static_cast<float>(height)) << label.value;
        }
    }

    EXPECT_FLOAT_EQ(glyph_heights[0U], 32.0F);
    // The compact 960px viewport keeps the requested 1.5x scale over the old 0.5 baseline.
    EXPECT_FLOAT_EQ(glyph_heights[1U], 24.0F);
}

TEST(DebugHudTest, CompactWindowKeepsTelemetryReadableWithinHalfTheCanvas)
{
    struct Viewport final {
        uint32_t width;
        uint32_t height;
        float dpi_scale;
        bool force_compact;
    };
    struct Footprint final {
        float left;
        float top;
        float right;
        float bottom;
    };
    auto const unionArea = [](std::vector<Footprint> const& footprints) {
        struct VerticalInterval final { float top; float bottom; };
        std::vector<float> x_edges;
        x_edges.reserve(footprints.size() * 2U);
        for (Footprint const& footprint : footprints) {
            x_edges.push_back(footprint.left);
            x_edges.push_back(footprint.right);
        }
        std::sort(x_edges.begin(), x_edges.end());
        x_edges.erase(std::unique(x_edges.begin(), x_edges.end()), x_edges.end());

        float area = 0.0F;
        for (size_t edge = 1U; edge < x_edges.size(); ++edge) {
            float const left = x_edges[edge - 1U];
            float const right = x_edges[edge];
            std::vector<VerticalInterval> intervals;
            for (Footprint const& footprint : footprints) {
                if (footprint.left < right && footprint.right > left) {
                    intervals.push_back({ footprint.top, footprint.bottom });
                }
            }
            std::sort(intervals.begin(), intervals.end(), [](auto const& lhs, auto const& rhs) {
                return lhs.top < rhs.top;
            });
            float covered_height = 0.0F;
            float interval_top = 0.0F;
            float interval_bottom = 0.0F;
            bool have_interval = false;
            for (VerticalInterval const& interval : intervals) {
                if (!have_interval) {
                    interval_top = interval.top;
                    interval_bottom = interval.bottom;
                    have_interval = true;
                } else if (interval.top <= interval_bottom) {
                    interval_bottom = (std::max)(interval_bottom, interval.bottom);
                } else {
                    covered_height += interval_bottom - interval_top;
                    interval_top = interval.top;
                    interval_bottom = interval.bottom;
                }
            }
            if (have_interval) {
                covered_height += interval_bottom - interval_top;
            }
            area += (right - left) * covered_height;
        }
        return area;
    };
    std::array<Viewport, 4U> const viewports{{
        { 1280U, 676U, 1.0F, true },
        { 844U, 346U, 1.0F, false },
        { 640U, 360U, 1.0F, false },
        { 1688U, 692U, 2.0F, false },
    }};
    client::DebugHudSnapshot const snapshot{
        .uptime_seconds = 86'400.0,
        .presented_fps = 60.0,
        .input = {
            .player_x = 37'888.0F,
            .player_y = 28'672.0F,
            .player_z = 84.1F,
            .camera_yaw_degrees = 180.0F,
            .camera_pitch_degrees = -42.0F,
            .camera_roll_degrees = 0.0F,
            .speedup = 1U,
            .selected_speedup = 99U,
            .acceleration_enabled = true,
            .flight_enabled = true,
            .collision_bypass_enabled = true,
            .loaded_dense_chunks_available = true,
            .loaded_dense_chunks = 513U,
            .client_memory_rss_bytes = 44'040'192U,
            .temperature_celsius = 16.6F,
            .target_block = shared::Block::Bedrock,
        },
        .enabled = true,
    };

    for (Viewport const viewport : viewports) {
        client::DebugHudLayout const layout = client::layoutDebugHud(
            viewport.width,
            viewport.height,
            viewport.dpi_scale,
            viewport.force_compact
        );
        ASSERT_TRUE(layout.compact);
        float const logical_width = static_cast<float>(viewport.width) / viewport.dpi_scale;
        float const logical_height = static_cast<float>(viewport.height) / viewport.dpi_scale;
        float const expected_font_scale = logical_width < 1050.0F || logical_height < 520.0F
            ? 0.75F
            : 1.0F;
        EXPECT_FLOAT_EQ(layout.glyph_scale / viewport.dpi_scale, expected_font_scale);

        client::TextRenderer labels;
        client::buildDebugHudPresentation(snapshot, layout, labels);
        auto const hasLabel = [&labels](std::string_view const value) {
            return std::any_of(labels.labels().begin(), labels.labels().end(), [value](auto const& label) {
                return label.value == value;
            });
        };
        auto const hasText = [&labels](std::string_view const value) {
            return std::any_of(labels.labels().begin(), labels.labels().end(), [value](auto const& label) {
                return label.value.find(value) != std::string::npos;
            });
        };
        EXPECT_TRUE(hasLabel("FPS 60"));
        EXPECT_TRUE(hasLabel("Up 1440:00"));
        EXPECT_TRUE(hasLabel("Cache 513"));
        EXPECT_TRUE(hasLabel("RSS 42 MiB"));
        EXPECT_TRUE(hasLabel("Position"));
        EXPECT_TRUE(hasLabel("X 37888.0"));
        EXPECT_TRUE(hasLabel("Z 28672.0"));
        EXPECT_TRUE(hasLabel("Y 84.1"));
        EXPECT_TRUE(hasText("X 37888.0"));
        EXPECT_TRUE(hasText("Z 28672.0"));
        EXPECT_TRUE(hasText("84.1"));
        EXPECT_FALSE(hasText("Target block Bedrock"));
        EXPECT_TRUE(hasText("YPR 180.0"));
        EXPECT_TRUE(hasLabel("Speed 1x"));
        EXPECT_TRUE(hasLabel("Selected 99x"));
        EXPECT_TRUE(hasLabel("Accel ON"));
        EXPECT_TRUE(hasLabel("Bypass ON"));
        EXPECT_TRUE(hasLabel("Flight ON"));
        EXPECT_TRUE(hasLabel("Temp 16.6 C"));
        EXPECT_TRUE(hasLabel("F5 View"));
        EXPECT_TRUE(hasLabel("F6 Move"));
        EXPECT_TRUE(hasLabel("G Mode"));
        EXPECT_TRUE(hasLabel("+/- Speed"));
        EXPECT_TRUE(hasLabel("Ctrl Accel"));
        EXPECT_FALSE(hasLabel("Game position example"));
        EXPECT_FALSE(hasLabel("Local climate"));

        std::vector<Footprint> footprints;
        footprints.push_back({
            layout.minimap.x,
            layout.minimap.y,
            layout.minimap.x + layout.minimap.width,
            layout.minimap.y + layout.minimap.height,
        });
        for (client::TextLabel const& label : labels.labels()) {
            float const text_width = static_cast<float>(label.value.size())
                * client::DEBUG_HUD_GLYPH_WIDTH_PIXELS * label.placement.scale;
            float const text_height = client::DEBUG_HUD_GLYPH_HEIGHT_PIXELS * label.placement.scale;
            EXPECT_GE(label.placement.position[0U], 0.0F) << label.value;
            EXPECT_GE(label.placement.position[1U], 0.0F) << label.value;
            EXPECT_LE(label.placement.position[0U] + text_width, static_cast<float>(viewport.width))
                << label.value;
            EXPECT_LE(label.placement.position[1U] + text_height, static_cast<float>(viewport.height))
                << label.value;

            footprints.push_back({
                label.placement.position[0U],
                label.placement.position[1U],
                label.placement.position[0U] + text_width,
                label.placement.position[1U] + text_height,
            });
        }
        float const covered_area = unionArea(footprints);
        float const canvas_area = static_cast<float>(viewport.width) * static_cast<float>(viewport.height);
        EXPECT_LT(covered_area, canvas_area * 0.5F);
    }
}

TEST(DebugHudTest, OutsideGameplayTheBuilderEmitsNoHudText)
{
    client::DebugHudSnapshot const snapshot{
        .input = {
            .gameplay_ui = false,
            .flight_enabled = true,
        },
        .enabled = true,
    };
    client::TextRenderer labels;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(1280U, 676U, 1.0F), labels);
    EXPECT_TRUE(labels.labels().empty());
}

TEST(DebugHudTest, HiddenGameplayHudEmitsNoLabels)
{
    client::DebugHudSnapshot const snapshot{
        .input = {
            .gameplay_ui = true,
            .flight_enabled = true,
            .collision_bypass_enabled = false,
            .target_block = shared::Block::Stone,
        },
        .enabled = false,
    };
    client::TextRenderer labels;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(1280U, 676U, 1.0F), labels);

    EXPECT_TRUE(labels.labels().empty());
}

TEST(DebugHudTest, EnabledGameplayHudKeepsGameControlHintsVisible)
{
    client::DebugHudSnapshot const snapshot{
        .input = {
            .flight_enabled = true,
            .collision_bypass_enabled = false,
        },
        .enabled = true,
    };
    client::TextRenderer labels;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(1280U, 676U, 1.0F), labels);

    auto const hasLabel = [&labels](std::string_view const value) {
        return std::any_of(labels.labels().begin(), labels.labels().end(), [value](auto const& label) {
            return label.value == value;
        });
    };
    EXPECT_TRUE(hasLabel("Flight ON"));
    EXPECT_TRUE(hasLabel("Bypass OFF"));
    EXPECT_TRUE(hasLabel("F1 Hide HUD"));
    EXPECT_TRUE(hasLabel("M Map"));
#if defined(__APPLE__) || defined(_WIN32)
    EXPECT_TRUE(hasLabel("F2 Screenshot"));
#else
    EXPECT_FALSE(hasLabel("F2 Screenshot"));
#endif
}


TEST(DebugHudTest, FixedPrototypeKeepsSemanticFacesAndDarkOutlinesByDefault)
{
    client::DebugHudSnapshot snapshot{.enabled = true};
    snapshot.input.text_style = client::TextStyle::Fixed;
    client::TextRenderer output;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(844U, 346U, 1.0F), output);
    auto const& labels = output.labels();
    ASSERT_FALSE(labels.empty());
    EXPECT_TRUE(std::all_of(labels.begin(), labels.end(), [](auto const& label) {
        return label.placement.style == client::TextStyle::Fixed;
    }));
    auto const face = std::find_if(labels.begin(), labels.end(), [](auto const& label) {
        return label.value == "Position" && label.color.packed() == 0xFF98'C7E5U;
    });
    ASSERT_NE(face, labels.end());
    EXPECT_EQ(std::count_if(labels.begin(), labels.end(), [](auto const& label) {
        return label.value == "Position" && label.color.packed() == 0xFF2D'2414U;
    }), 4);
}

TEST(DebugHudTest, AdaptivePrototypeUsesOneFacePerLabelAndPreservesMixedStyleGlyphs)
{
    auto const layout = client::layoutDebugHud(844U, 346U, 1.0F);
    client::DebugHudSnapshot snapshot{.enabled = true};
    snapshot.input.text_style = client::TextStyle::Fixed;
    client::TextRenderer fixed;
    client::buildDebugHudPresentation(snapshot, layout, fixed);
    snapshot.input.text_style = client::TextStyle::Adaptive;
    client::TextRenderer adaptive;
    client::buildDebugHudPresentation(snapshot, layout, adaptive);
    ASSERT_FALSE(adaptive.labels().empty());
    EXPECT_EQ(fixed.labels().size(), adaptive.labels().size() * 5U);
    bool found_style_hint = false;
    for (auto const& face : adaptive.labels()) {
        std::string_view const fixed_value = face.value == "F7 Colored HUD"
            ? "F7 Experimental contrast"
            : std::string_view{face.value};
        auto const fixed_face = std::find_if(
            fixed.labels().begin(), fixed.labels().end(), [fixed_value, &face](auto const& label) {
                return label.value == fixed_value && label.placement.style == client::TextStyle::Fixed
                    && label.color.packed() == face.placement.light_color.packed();
            }
        );
        ASSERT_NE(fixed_face, fixed.labels().end());
        if (face.value == "F7 Colored HUD") {
            found_style_hint = true;
        }
        EXPECT_EQ(face.placement.position, fixed_face->placement.position);
        EXPECT_EQ(face.placement.style, client::TextStyle::Adaptive);
        EXPECT_EQ(face.placement.light_color.packed(), fixed_face->color.packed());
        EXPECT_EQ(face.placement.dark_color.packed(), 0xFF2D'2414U);
    }
    EXPECT_TRUE(found_style_hint);
    client::TextRenderer mixed;
    static_cast<void>(mixed.submit("Map", {}, {}));
    for (auto const& label : adaptive.labels()) {
        static_cast<void>(mixed.submit(label.value, label.color, label.placement));
    }
    client::GuiRenderer gui;
    ASSERT_TRUE(gui.submit(mixed, glm::mat4{1.0F}, 844U, 346U));
    ASSERT_GT(gui.batches().size(), 1U);
    auto const& fixed_batch = gui.batches()[0U];
    auto const& adaptive_batch = gui.batches()[1U];
    ASSERT_FALSE(fixed_batch.glyphs.empty());
    ASSERT_FALSE(adaptive_batch.glyphs.empty());
    EXPECT_EQ(fixed_batch.glyphs[0U].character & 0x100U, 0U);
    EXPECT_NE(adaptive_batch.glyphs[0U].character & 0x100U, 0U);
    EXPECT_EQ(adaptive_batch.glyphs[0U].light_color, adaptive.labels()[0U].placement.light_color.packed());
    EXPECT_EQ(adaptive_batch.glyphs[0U].dark_color, 0xFF2D'2414U);
}

TEST(DebugHudTest, DefaultColoredContrastShowsCurrentInterestAndCachedTotal)
{
    client::DebugHudSnapshot snapshot{.enabled = true};
    snapshot.input.loaded_dense_chunks_available = true;
    snapshot.input.loaded_dense_chunks = 1024U;
    snapshot.input.current_interest_chunks_available = true;
    snapshot.input.current_interest_resident_chunks = 37U;
    snapshot.input.current_interest_required_chunks = 72U;
    snapshot.input.ready_terrain_columns = 2786U;
    snapshot.input.required_terrain_columns = 6561U;
    client::TextRenderer output;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(1280U, 676U, 1.0F), output);
    std::array<uint32_t, 3U> axes{};
    bool chunk_status = false;
    for (auto const& label : output.labels()) {
        EXPECT_TRUE(label.placement.style == client::TextStyle::ColoredAdaptive
            || label.placement.style == client::TextStyle::AdaptiveHalo);
        if (label.value.starts_with("X ")) { axes[0] = label.color.packed(); }
        if (label.value.starts_with("Y ")) { axes[1] = label.color.packed(); }
        if (label.value.starts_with("Z ")) { axes[2] = label.color.packed(); }
        chunk_status = chunk_status || label.value == "Chunks 37 | Cache 1024";
    }
    EXPECT_TRUE(chunk_status);
    EXPECT_TRUE(std::any_of(output.labels().begin(), output.labels().end(), [](auto const& label) {
        return label.value == "Terrain 2786/6561";
    }));
    EXPECT_FALSE(std::any_of(output.labels().begin(), output.labels().end(), [](auto const& label) {
        return label.value.find("Active interest") != std::string::npos
            || label.value.find("ready/required") != std::string::npos
            || label.value.find("Terrain ready") != std::string::npos;
    }));
    EXPECT_NE(axes[0], 0U);
    EXPECT_NE(axes[1], 0U);
    EXPECT_NE(axes[2], 0U);
    EXPECT_NE(axes[0], axes[1]);
    EXPECT_NE(axes[0], axes[2]);
    EXPECT_NE(axes[1], axes[2]);
}

TEST(DebugHudTest, WrappedSessionCountersDoNotOverlapInNarrowCompactWindow)
{
    client::DebugHudSnapshot snapshot{.enabled = true};
    snapshot.input.loaded_dense_chunks_available = true;
    snapshot.input.loaded_dense_chunks = 1024U;
    snapshot.input.current_interest_chunks_available = true;
    snapshot.input.current_interest_resident_chunks = 37U;
    snapshot.input.current_interest_required_chunks = 72U;
    snapshot.input.ready_terrain_columns = 2786U;
    snapshot.input.required_terrain_columns = 6561U;
    client::TextRenderer output;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(400U, 600U, 1.0F, true), output);
    float heading_y = 600.0F;
    for (auto const& label : output.labels()) {
        if (label.value == "Position") { heading_y = label.placement.position[1]; }
    }
    auto const labels = output.labels();
    for (size_t index = 0U; index < labels.size(); ++index) {
        auto const& first = labels[index];
        if (first.placement.position[1] >= heading_y
            || first.placement.style == client::TextStyle::AdaptiveHalo) { continue; }
        for (size_t other = index + 1U; other < labels.size(); ++other) {
            auto const& second = labels[other];
            if (second.placement.position[1] >= heading_y
                || second.placement.style == client::TextStyle::AdaptiveHalo) { continue; }
            float const first_right = first.placement.position[0]
                + static_cast<float>(first.value.size()) * client::DEBUG_HUD_GLYPH_WIDTH_PIXELS * first.placement.scale;
            float const second_right = second.placement.position[0]
                + static_cast<float>(second.value.size()) * client::DEBUG_HUD_GLYPH_WIDTH_PIXELS * second.placement.scale;
            bool const disjoint = first_right <= second.placement.position[0]
                || second_right <= first.placement.position[0]
                || first.placement.position[1]
                    + client::DEBUG_HUD_GLYPH_HEIGHT_PIXELS * first.placement.scale
                    <= second.placement.position[1]
                || second.placement.position[1]
                    + client::DEBUG_HUD_GLYPH_HEIGHT_PIXELS * second.placement.scale
                    <= first.placement.position[1];
            EXPECT_TRUE(disjoint) << first.value << " overlaps " << second.value;
        }
    }
}

TEST(DebugHudTest, ColoredAxesShareOneRowAndUnavailableHumidityIsOmitted)
{
    client::DebugHudSnapshot snapshot{.enabled = true};
    snapshot.input.text_style = client::TextStyle::ColoredAdaptive;
    snapshot.input.player_x = 12'345.6F;
    snapshot.input.player_y = -12'345.6F;
    snapshot.input.player_z = 123.4F;
    for (uint32_t width : {400U, 844U, 1280U}) {
        client::TextRenderer output;
        client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(width, 600U, 1.0F, width < 900U), output);
        std::array<float, 3U> row_y{};
        std::array<float, 3U> speed_y{};
        std::array<uint32_t, 3U> colors{};
        std::array<bool, 3U> axes_found{};
        for (auto const& label : output.labels()) {
            EXPECT_EQ(label.value.find("Humidity"), std::string::npos);
            if (label.color.packed() == 0xFF2D'2414U) { continue; }
            if (label.value.starts_with("Speed ")) { speed_y[0] = label.placement.position[1]; }
            if (label.value.starts_with("Selected ")) { speed_y[1] = label.placement.position[1]; }
            if (label.value.starts_with("Accel ")) { speed_y[2] = label.placement.position[1]; }
            for (size_t axis = 0U; axis < 3U; ++axis) {
                if (label.value.starts_with(std::string{static_cast<char>('X' + axis)} + " ")) {
                    row_y[axis] = label.placement.position[1];
                    colors[axis] = label.color.packed();
                    axes_found[axis] = true;
                }
            }
        }
        if (width >= 1280U) {
            EXPECT_TRUE(std::all_of(axes_found.begin(), axes_found.end(), [](bool const found) {
                return found;
            }));
            EXPECT_FLOAT_EQ(speed_y[0], speed_y[1]);
            EXPECT_FLOAT_EQ(speed_y[1], speed_y[2]);
            EXPECT_FLOAT_EQ(row_y[0], row_y[1]);
            EXPECT_FLOAT_EQ(row_y[1], row_y[2]);
            EXPECT_NE(colors[0], colors[1]);
            EXPECT_NE(colors[0], colors[2]);
            EXPECT_NE(colors[1], colors[2]);
        }
        for (auto const& label : output.labels()) {
            bool const readout = label.value.starts_with("X ") || label.value.starts_with("Y ")
                || label.value.starts_with("Z ") || label.value.starts_with("Speed ")
                || label.value.starts_with("Selected ") || label.value.starts_with("Accel ");
            if (readout && label.placement.style == client::TextStyle::ColoredAdaptive) {
                EXPECT_GE(label.placement.scale, width < 900U ? 0.375F : 0.75F);
            }
        }
    }
}

TEST(DebugHudTest, OmitsUnavailableGameplayTelemetryButPreservesKnownZeroValues)
{
    client::DebugHudSnapshot snapshot{};
    snapshot.enabled = true;
    for (auto const size : std::array<std::array<uint32_t, 2U>, 2U>{{{844U, 346U}, {1920U, 1080U}}}) {
        auto const layout = client::layoutDebugHud(size[0], size[1], 1.0F, false);
        client::TextRenderer labels;
        client::buildDebugHudPresentation(snapshot, layout, labels);
        for (auto const& label : labels.labels()) {
            EXPECT_EQ(label.value.find("Chunks"), std::string::npos);
            EXPECT_EQ(label.value.find("Terrain ready"), std::string::npos);
            EXPECT_EQ(label.value.find("RSS"), std::string::npos);
            EXPECT_EQ(label.value.find("Memory"), std::string::npos);
            EXPECT_EQ(label.value.find("Temp"), std::string::npos);
            EXPECT_EQ(label.value.find("unavailable"), std::string::npos);
        }
    }
    snapshot.input.loaded_dense_chunks_available = true;
    snapshot.input.client_memory_rss_bytes = 0U;
    snapshot.input.temperature_celsius = 0.0F;
    client::TextRenderer labels;
    client::buildDebugHudPresentation(snapshot, client::layoutDebugHud(844U, 346U, 1.0F, false), labels);
    auto const contains = [&labels](std::string_view value) {
        return std::ranges::any_of(labels.labels(), [value](auto const& label) { return label.value == value; });
    };
    EXPECT_TRUE(contains("Cache 0"));
    EXPECT_TRUE(contains("RSS 0 MiB"));
    EXPECT_TRUE(contains("Temp 0.0 C"));
}
