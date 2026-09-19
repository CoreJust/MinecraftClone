#pragma once

#include <client/render/TextRenderer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace client {

inline constexpr size_t DEBUG_HUD_LINE_COUNT = 5U;
inline constexpr std::array<size_t, DEBUG_HUD_LINE_COUNT> DEBUG_HUD_LINE_BYTES{
    32U,
    32U,
    32U,
    64U,
    64U,
};
inline constexpr std::array<size_t, DEBUG_HUD_LINE_COUNT> DEBUG_HUD_LINE_WORD_OFFSETS{
    0U,
    8U,
    16U,
    24U,
    40U,
};
inline constexpr size_t DEBUG_HUD_MAX_TEXT_BYTES = 228U;
inline constexpr size_t DEBUG_HUD_MAX_INSTANCES = 56U;
inline constexpr size_t DEBUG_HUD_MAX_PRESENTED_SAMPLES = 256;
inline constexpr double DEBUG_HUD_FPS_WINDOW_SECONDS = 1.0;
inline constexpr float DEBUG_HUD_BITMAP_GLYPH_WIDTH_PIXELS = 8.0F;
inline constexpr float DEBUG_HUD_BITMAP_GLYPH_HEIGHT_PIXELS = 16.0F;
inline constexpr float DEBUG_HUD_GLYPH_SCALE = 2.0F;
inline constexpr float DEBUG_HUD_GLYPH_WIDTH_PIXELS =
    DEBUG_HUD_BITMAP_GLYPH_WIDTH_PIXELS * DEBUG_HUD_GLYPH_SCALE;
inline constexpr float DEBUG_HUD_GLYPH_HEIGHT_PIXELS =
    DEBUG_HUD_BITMAP_GLYPH_HEIGHT_PIXELS * DEBUG_HUD_GLYPH_SCALE;
inline constexpr float DEBUG_HUD_MARGIN_PIXELS = 8.0F;
inline constexpr float DEBUG_HUD_LINE_ADVANCE_PIXELS = 40.0F;

struct DebugHudClock {
    using NowFunction = double (*)(void*) noexcept;

    NowFunction now = nullptr;
    void* context = nullptr;
};

struct DebugHudNumberFormatter {
    using FormatFunction = size_t (*)(
        char*,
        size_t,
        double,
        int,
        void*
    ) noexcept;

    FormatFunction format = nullptr;
    void* context = nullptr;
};

struct DebugHudInput {
    bool presented = false;
    bool touch_flight_help = false;
    float player_x = 0.0F;
    float player_y = 0.0F;
    float player_z = 0.0F;
    float camera_yaw_degrees = 0.0F;
    float camera_pitch_degrees = 0.0F;
    float camera_roll_degrees = 0.0F;
    uint16_t speedup = 1U;
    uint16_t selected_speedup = 5U;
    bool acceleration_enabled = false;
};

using DebugHudText = TextDocument;
using DebugHudInstance = TextGlyph;
using DebugHudBatch = TextBatch;

enum class DebugHudColor : uint8_t {
    White,
    Cyan,
    Gold,
    Green,
    Rose,
};

struct DebugHudSnapshot {
    double uptime_seconds = 0.0;
    double presented_fps = 0.0;
    DebugHudInput input{};
    float dpi_scale = 1.0F;
    bool enabled = false;
};

// Shared edge detector for the shipped performance-toggle input. Keeping the
// latch in the HUD boundary makes desktop and Android debounce the same key
// transition without allocating or introducing another platform key binding.
class DebugHudToggleLatch final {
public:
    [[nodiscard]] bool update(bool pressed) noexcept;
    void reset() noexcept;

private:
    bool pressed_ = false;
};

[[nodiscard]] uint8_t sanitizeDebugHudByte(uint8_t byte) noexcept;

[[nodiscard]] uint32_t packDebugHudAscii(
    uint8_t c0,
    uint8_t c1,
    uint8_t c2,
    uint8_t c3
) noexcept;

void packDebugHudText(DebugHudText const& text, DebugHudBatch& batch);

class DebugHudState final {
public:
    explicit DebugHudState(
        DebugHudClock clock = {},
        DebugHudNumberFormatter formatter = {}
    ) noexcept;

    void update(DebugHudInput const& input) noexcept;
    void updateAt(double monotonic_seconds, DebugHudInput const& input) noexcept;

    void setEnabled(bool enabled) noexcept;
    void toggle() noexcept;
    void setDpiScale(float dpi_scale) noexcept;
    void setLineColor(size_t line, DebugHudColor color) noexcept;

    [[nodiscard]] DebugHudSnapshot snapshot() const noexcept;
    [[nodiscard]] bool formatText(DebugHudText& text) const;
    [[nodiscard]] std::array<TextColor, DEBUG_HUD_LINE_COUNT> lineColors() const noexcept;
    [[nodiscard]] bool buildBatch(DebugHudText& text, DebugHudBatch& batch) const;

private:
    std::array<DebugHudColor, DEBUG_HUD_LINE_COUNT> line_colors_{
        DebugHudColor::White,
        DebugHudColor::White,
        DebugHudColor::White,
        DebugHudColor::White,
        DebugHudColor::White,
    };
    static double defaultNow(void*) noexcept;

    [[nodiscard]] size_t appendText(
        DebugHudText& text,
        size_t offset,
        size_t limit,
        std::string_view value,
        TextColor color
    ) const;
    [[nodiscard]] size_t appendNumber(
        DebugHudText& text,
        size_t offset,
        size_t limit,
        double value,
        int decimals,
        TextColor color
    ) const;
    void recordPresentation(double monotonic_seconds, bool presented) noexcept;

    DebugHudClock clock_{};
    DebugHudNumberFormatter formatter_{};
    std::array<double, DEBUG_HUD_MAX_PRESENTED_SAMPLES> presented_times_{};
    size_t presented_begin_ = 0;
    size_t presented_size_ = 0;
    double start_time_ = 0.0;
    double now_ = 0.0;
    DebugHudInput input_{};
    float dpi_scale_ = 1.0F;
    bool enabled_ = false;
    bool has_time_ = false;
};

} // namespace client
