#include <client/render/DebugHud.hpp>
#include <client/render/TextRenderer.hpp>

#include <gtest/gtest.h>

#include <array>
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

TEST(DebugHudTest, FormatsBoundedTelemetryViewAndAuthorityLines)
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
    });

    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    EXPECT_EQ(
        std::string_view(text.value),
        "FPS:0.0\nUPTIME:0.0s\nSPEED:1x(5x)\nVIEW: FIRST (F5 or 5)\n"
        "FLIGHT: OFF BYPASS: OFF (F6 or 6)\nXYZ: 1.2 4.6 -2.3\nYPR deg: 45.0 -10.0 3.0"
    );
    ASSERT_EQ(text.spans.size(), 12U);
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

    expectSpan({ .index = 0U, .offset = 0U, .length = 94U, .color = white });
    expectSpan({ .index = 1U, .offset = 94U, .length = 3U, .color = cyan });
    expectSpan({ .index = 2U, .offset = 97U, .length = 1U, .color = white });
    expectSpan({ .index = 3U, .offset = 98U, .length = 3U, .color = gold });
    expectSpan({ .index = 4U, .offset = 101U, .length = 1U, .color = white });
    expectSpan({ .index = 5U, .offset = 102U, .length = 4U, .color = green });
    expectSpan({ .index = 6U, .offset = 106U, .length = 10U, .color = white });
    expectSpan({ .index = 7U, .offset = 116U, .length = 4U, .color = cyan });
    expectSpan({ .index = 8U, .offset = 120U, .length = 1U, .color = white });
    expectSpan({ .index = 9U, .offset = 121U, .length = 5U, .color = gold });
    expectSpan({ .index = 10U, .offset = 126U, .length = 1U, .color = white });
    expectSpan({ .index = 11U, .offset = 127U, .length = 3U, .color = rose });
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
    EXPECT_NE(formatted.find("SPEED:30x(ON)"), std::string_view::npos);
}

TEST(DebugHudTest, NamesEveryCameraViewAndAuthorityWithoutGameplayArchetypes)
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
    EXPECT_NE(formatted.find("VIEW: FRONT (F5 or 5)"), std::string_view::npos);
    EXPECT_NE(formatted.find("FLIGHT: ON BYPASS: ON (F6 or 6)"), std::string_view::npos);
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
    EXPECT_EQ(batch.glyphs.size(), text.value.size() - 6U);
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

TEST(DebugHudTest, UsesPresentedFramesInSlidingWindow)
{
    client::DebugHudState hud;
    client::DebugHudInput input{ .presented = true };

    hud.updateAt(0.0, input);
    hud.updateAt(0.5, input);
    hud.updateAt(1.0, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 2.0);

    input.presented = false;
    hud.updateAt(2.1, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
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
