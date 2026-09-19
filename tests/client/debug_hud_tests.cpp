#include <client/render/DebugHud.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string_view>

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

TEST(DebugHudTest, FormatsFiveBoundedColoredLines)
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
        "FPS:  0.0\nUPTIME:  0.0s\nSPEED:1x(5x)\nXYZ: 1.2 4.6 -2.3\nYPR deg: 45.0 -10.0 3.0"
    );
    ASSERT_EQ(text.spans.size(), 12U);
    uint32_t const white = client::TextColor{}.packed();
    uint32_t const cyan = client::TextColor{ 0.48F, 0.88F, 1.0F, 1.0F }.packed();
    uint32_t const gold = client::TextColor{ 1.0F, 0.82F, 0.38F, 1.0F }.packed();
    uint32_t const green = client::TextColor{ 0.63F, 1.0F, 0.62F, 1.0F }.packed();
    uint32_t const rose = client::TextColor{ 1.0F, 0.65F, 0.74F, 1.0F }.packed();
    auto expectSpan = [&](size_t const index, size_t const offset, size_t const length, uint32_t const color) {
        ASSERT_LT(index, text.spans.size());
        EXPECT_EQ(text.spans[index].offset, offset);
        EXPECT_EQ(text.spans[index].length, length);
        EXPECT_EQ(text.spans[index].packed_color, color);
    };

    expectSpan(0U, 0U, 42U, white);
    expectSpan(1U, 42U, 3U, cyan);
    expectSpan(2U, 45U, 1U, white);
    expectSpan(3U, 46U, 3U, gold);
    expectSpan(4U, 49U, 1U, white);
    expectSpan(5U, 50U, 4U, green);
    expectSpan(6U, 54U, 10U, white);
    expectSpan(7U, 64U, 4U, cyan);
    expectSpan(8U, 68U, 1U, white);
    expectSpan(9U, 69U, 5U, gold);
    expectSpan(10U, 74U, 1U, white);
    expectSpan(11U, 75U, 3U, rose);
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

TEST(DebugHudTest, BuildsDynamicGlyphBatch)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    client::DebugHudText text;
    client::DebugHudBatch batch;

    ASSERT_TRUE(hud.buildBatch(text, batch));
    EXPECT_EQ(batch.space, client::TextSpace::Screen);
    EXPECT_EQ(batch.glyphs.size(), text.value.size() - 4U);
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
    EXPECT_NE(std::string_view(text.value).find("FPS:  X"), std::string_view::npos);
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
