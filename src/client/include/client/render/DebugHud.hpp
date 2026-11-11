#pragma once

#include <client/render/PresentationFrameProgress.hpp>
#include <client/render/TextRenderer.hpp>
#include <shared/world/Chunk.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace client {

inline constexpr size_t DEBUG_HUD_LINE_COUNT = 8U;
inline constexpr std::array<size_t, DEBUG_HUD_LINE_COUNT> DEBUG_HUD_LINE_BYTES{
    40U,
    32U,
    32U,
    40U,
    64U,
    64U,
    48U,
    64U,
};
inline constexpr std::array<size_t, DEBUG_HUD_LINE_COUNT> DEBUG_HUD_LINE_WORD_OFFSETS{
    0U,
    10U,
    18U,
    26U,
    36U,
    52U,
    68U,
    80U,
};
inline constexpr size_t DEBUG_HUD_MAX_TEXT_BYTES = 392U;
inline constexpr size_t DEBUG_HUD_MAX_INSTANCES = 98U;
inline constexpr size_t DEBUG_HUD_MAX_PRESENTED_SAMPLES = 512U;
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
    TextStyle text_style = TextStyle::ColoredAdaptive;
    bool gameplay_ui = true;
    bool compact_window = false;
    // The renderer writes this legacy status for other diagnostics; FPS does not consume it.
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
    uint8_t view_index = 0U;
    bool flight_enabled = false;
    bool collision_bypass_enabled = false;
    std::optional<RendererFrameIdentity> completed_frame;
    // Missing or expired fence observations do not contribute to FPS.
    std::optional<std::chrono::steady_clock::time_point> completed_frame_observed_at;
    bool loaded_dense_chunks_available = false;
    uint64_t loaded_dense_chunks = 0U;
    bool current_interest_chunks_available = false;
    uint32_t current_interest_resident_chunks = 0U;
    uint32_t current_interest_required_chunks = 0U;
    uint32_t ready_terrain_columns = 0U;
    uint32_t required_terrain_columns = 0U;
    std::optional<uint64_t> client_memory_rss_bytes;
    std::optional<float> temperature_celsius;
    // Crosshair hit material, distinct from the selected placement material.
    std::optional<shared::Block> target_block = std::nullopt;
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

struct DebugHudRect final {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct DebugHudPoint final {
    float x = 0.0F;
    float y = 0.0F;
};

struct DebugHudLayout final {
    bool compact = false;
    float dpi_scale = 1.0F;
    float framebuffer_width_pixels = 0.0F;
    float framebuffer_height_pixels = 0.0F;
    float glyph_scale = 1.0F;
    float small_glyph_scale = 0.5F;
    float outline_offset_pixels = 1.0F;
    float margin_pixels = 0.0F;
    float gap_pixels = 0.0F;
    float left_column_x = 0.0F;
    float right_column_x = 0.0F;
    float column_width_pixels = 0.0F;
    float selected_block_y = 0.0F;
    float controls_y = 0.0F;
    float control_line_advance_pixels = 0.0F;
    DebugHudRect minimap{};
    DebugHudRect minimap_interactive{};
    DebugHudPoint north{};
    DebugHudPoint east{};
    DebugHudPoint south{};
    DebugHudPoint west{};
    DebugHudPoint heading{};
};

[[nodiscard]] DebugHudLayout layoutDebugHud(
    uint32_t framebuffer_width,
    uint32_t framebuffer_height,
    float dpi_scale,
    bool force_compact = false
) noexcept;

void buildDebugHudPresentation(
    DebugHudSnapshot const& snapshot,
    DebugHudLayout const& layout,
    TextRenderer& output
);

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
    struct AppendRange final {
        size_t offset = 0U;
        size_t limit = 0U;
    };

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
        AppendRange range,
        std::string_view value,
        TextColor color
    ) const;
    [[nodiscard]] size_t appendNumber(
        DebugHudText& text,
        AppendRange range,
        double value,
        int decimals,
        TextColor color
    ) const;
    void recordPresentation(
        double monotonic_seconds,
        std::optional<RendererFrameIdentity> const& completed_frame,
        std::optional<std::chrono::steady_clock::time_point> const& observed_at
    ) noexcept;

    DebugHudClock clock_{};
    DebugHudNumberFormatter formatter_{};
    std::array<double, DEBUG_HUD_MAX_PRESENTED_SAMPLES> presented_times_{};
    size_t presented_begin_ = 0;
    size_t presented_size_ = 0;
    double start_time_ = 0.0;
    double now_ = 0.0;
    DebugHudInput input_{};
    std::optional<RendererFrameIdentity> last_completed_frame_;
    float dpi_scale_ = 1.0F;
    bool enabled_ = false;
    bool has_time_ = false;
};

} // namespace client
