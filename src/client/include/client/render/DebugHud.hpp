#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace client {

inline constexpr size_t DEBUG_HUD_MAX_TEXT_BYTES = 112;
inline constexpr size_t DEBUG_HUD_MAX_INSTANCES =
    (DEBUG_HUD_MAX_TEXT_BYTES + 3) / 4;
inline constexpr size_t DEBUG_HUD_MAX_PRESENTED_SAMPLES = 256;
inline constexpr double DEBUG_HUD_FPS_WINDOW_SECONDS = 1.0;

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
    float player_x = 0.0F;
    float player_y = 0.0F;
    float player_z = 0.0F;
    float camera_yaw_degrees = 0.0F;
    float camera_pitch_degrees = 0.0F;
    float camera_roll_degrees = 0.0F;
};

struct DebugHudText {
    std::array<char, DEBUG_HUD_MAX_TEXT_BYTES> bytes{};
    size_t size = 0;
};

struct DebugHudInstance {
    uint32_t packed_ascii = 0;
};

struct DebugHudBatch {
    std::array<DebugHudInstance, DEBUG_HUD_MAX_INSTANCES> instances{};
    size_t size = 0;
};

struct DebugHudSnapshot {
    double uptime_seconds = 0.0;
    double presented_fps = 0.0;
    DebugHudInput input{};
    float dpi_scale = 1.0F;
    bool enabled = true;
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

void packDebugHudText(DebugHudText const& text, DebugHudBatch& batch) noexcept;

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

    [[nodiscard]] DebugHudSnapshot snapshot() const noexcept;
    [[nodiscard]] bool formatText(DebugHudText& text) const noexcept;
    [[nodiscard]] bool buildBatch(DebugHudText& text, DebugHudBatch& batch) const noexcept;

private:
    static double defaultNow(void*) noexcept;

    [[nodiscard]] size_t appendText(
        DebugHudText& text,
        size_t offset,
        std::string_view value
    ) const noexcept;
    [[nodiscard]] size_t appendNumber(
        DebugHudText& text,
        size_t offset,
        double value,
        int decimals
    ) const noexcept;
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
    bool enabled_ = true;
    bool has_time_ = false;
};

} // namespace client
