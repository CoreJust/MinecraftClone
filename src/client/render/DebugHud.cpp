#include <client/render/DebugHud.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace client {

namespace {

constexpr float MIN_DPI_SCALE = 0.25F;
constexpr float MAX_DPI_SCALE = 8.0F;

[[nodiscard]] TextColor hudColor(DebugHudColor const color) noexcept {
    switch (color) {
    case DebugHudColor::White: return { 1.0F, 1.0F, 1.0F, 1.0F };
    case DebugHudColor::Cyan: return { 0.48F, 0.88F, 1.0F, 1.0F };
    case DebugHudColor::Gold: return { 1.0F, 0.82F, 0.38F, 1.0F };
    case DebugHudColor::Green: return { 0.63F, 1.0F, 0.62F, 1.0F };
    case DebugHudColor::Rose: return { 1.0F, 0.65F, 0.74F, 1.0F };
    }
    return {};
}

[[nodiscard]] size_t formatNumberWithToChars(
    char* const destination,
    size_t const capacity,
    double const value,
    int const decimals,
    void*
) noexcept {
    std::to_chars_result const result =
        std::to_chars(destination, destination + capacity, value, std::chars_format::fixed, decimals);
    if (result.ec != std::errc{}) {
        return 0;
    }
    return static_cast<size_t>(result.ptr - destination);
}

} // namespace

bool DebugHudToggleLatch::update(bool const pressed) noexcept {
    bool const toggled = pressed && !pressed_;
    pressed_ = pressed;
    return toggled;
}

void DebugHudToggleLatch::reset() noexcept {
    pressed_ = false;
}

uint8_t sanitizeDebugHudByte(uint8_t const byte) noexcept {
    return sanitizeTextByte(byte);
}

uint32_t packDebugHudAscii(
    uint8_t const c0,
    uint8_t const c1,
    uint8_t const c2,
    uint8_t const c3
) noexcept {
    return static_cast<uint32_t>(sanitizeDebugHudByte(c0))
        | (static_cast<uint32_t>(sanitizeDebugHudByte(c1)) << 8U)
        | (static_cast<uint32_t>(sanitizeDebugHudByte(c2)) << 16U)
        | (static_cast<uint32_t>(sanitizeDebugHudByte(c3)) << 24U);
}

void packDebugHudText(DebugHudText const& text, DebugHudBatch& batch) {
    packText(text, batch);
}

DebugHudState::DebugHudState(
    DebugHudClock const clock,
    DebugHudNumberFormatter const formatter
) noexcept
    : clock_(clock)
    , formatter_(formatter)
{
    if (clock_.now == nullptr) {
        clock_.now = &DebugHudState::defaultNow;
    }
    if (formatter_.format == nullptr) {
        formatter_.format = &formatNumberWithToChars;
    }
}

void DebugHudState::update(DebugHudInput const& input) noexcept {
    updateAt(clock_.now(clock_.context), input);
}

void DebugHudState::updateAt(
    double const monotonic_seconds,
    DebugHudInput const& input
) noexcept {
    double const finite_seconds = std::isfinite(monotonic_seconds)
        ? monotonic_seconds
        : (has_time_ ? now_ : 0.0);
    double const monotonic_now = has_time_
        ? std::max(finite_seconds, now_)
        : finite_seconds;
    if (!has_time_) {
        start_time_ = monotonic_now;
        has_time_ = true;
    }
    now_ = monotonic_now;
    input_ = input;
    recordPresentation(monotonic_now, input.presented);
}

void DebugHudState::setEnabled(bool const enabled) noexcept {
    enabled_ = enabled;
}

void DebugHudState::toggle() noexcept {
    enabled_ = !enabled_;
}

void DebugHudState::setDpiScale(float const dpi_scale) noexcept {
    if (!std::isfinite(dpi_scale)) {
        return;
    }
    dpi_scale_ = std::clamp(dpi_scale, MIN_DPI_SCALE, MAX_DPI_SCALE);
}

void DebugHudState::setLineColor(size_t const line, DebugHudColor const color) noexcept {
    if (line < line_colors_.size() && color <= DebugHudColor::Rose) {
        line_colors_[line] = color;
    }
}

DebugHudSnapshot DebugHudState::snapshot() const noexcept {
    double const duration = presented_size_ < 2
        ? 0.0
        : presented_times_[(presented_begin_ + presented_size_ - 1) % presented_times_.size()]
            - presented_times_[presented_begin_];
    return {
        .uptime_seconds = has_time_ ? now_ - start_time_ : 0.0,
        .presented_fps = duration <= 0.0
            ? 0.0
            : static_cast<double>(presented_size_ - 1) / duration,
        .input = input_,
        .dpi_scale = dpi_scale_,
        .enabled = enabled_,
    };
}

std::array<TextColor, DEBUG_HUD_LINE_COUNT> DebugHudState::lineColors() const noexcept {
    std::array<TextColor, DEBUG_HUD_LINE_COUNT> colors{};
    for (size_t line = 0U; line < colors.size(); ++line) {
        colors[line] = hudColor(line_colors_[line]);
    }
    return colors;
}

bool DebugHudState::formatText(DebugHudText& text) const {
    clearText(text);
    if (!enabled_) {
        return false;
    }

    DebugHudSnapshot const values = snapshot();
    size_t offset = 0U;
    size_t line_limit = offset + DEBUG_HUD_LINE_BYTES[0];
    offset = appendText(text, offset, line_limit, "FPS:  ", hudColor(line_colors_[0]));
    offset = appendNumber(text, offset, line_limit, values.presented_fps, 1, hudColor(line_colors_[0]));
    offset = appendText(text, offset, text.size() + 1U, "\n", hudColor(line_colors_[0]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[1];
    if (values.input.touch_flight_help) {
        offset = appendText(text, offset, line_limit, "TOUCH: UP/DOWN", hudColor(line_colors_[1]));
    } else {
        offset = appendText(text, offset, line_limit, "UPTIME:  ", hudColor(line_colors_[1]));
        offset = appendNumber(text, offset, line_limit, values.uptime_seconds, 1, hudColor(line_colors_[1]));
        offset = appendText(text, offset, line_limit, "s", hudColor(line_colors_[1]));
    }
    offset = appendText(text, offset, text.size() + 1U, "\n", hudColor(line_colors_[1]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[2];
    offset = appendText(text, offset, line_limit, "SPEED:", hudColor(line_colors_[2]));
    offset = appendNumber(text, offset, line_limit, values.input.speedup, 0, hudColor(line_colors_[2]));
    offset = appendText(text, offset, line_limit, "x(", hudColor(line_colors_[2]));
    if (values.input.acceleration_enabled) {
        offset = appendText(text, offset, line_limit, "ON", hudColor(line_colors_[2]));
    } else {
        offset = appendNumber(text, offset, line_limit, values.input.selected_speedup, 0, hudColor(line_colors_[2]));
        offset = appendText(text, offset, line_limit, "x", hudColor(line_colors_[2]));
    }
    offset = appendText(text, offset, line_limit, ")", hudColor(line_colors_[2]));
    offset = appendText(text, offset, text.size() + 1U, "\n", hudColor(line_colors_[2]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[3];
    TextColor const white = hudColor(DebugHudColor::White);
    offset = appendText(text, offset, line_limit, "XYZ: ", white);
    offset = appendNumber(text, offset, line_limit, values.input.player_x, 1, hudColor(DebugHudColor::Cyan));
    offset = appendText(text, offset, line_limit, " ", white);
    offset = appendNumber(text, offset, line_limit, values.input.player_z, 1, hudColor(DebugHudColor::Gold));
    offset = appendText(text, offset, line_limit, " ", white);
    offset = appendNumber(text, offset, line_limit, values.input.player_y, 1, hudColor(DebugHudColor::Green));
    offset = appendText(text, offset, text.size() + 1U, "\n", white);

    line_limit = offset + DEBUG_HUD_LINE_BYTES[4];
    offset = appendText(text, offset, line_limit, "YPR deg: ", white);
    offset = appendNumber(text, offset, line_limit, values.input.camera_yaw_degrees, 1, hudColor(DebugHudColor::Cyan));
    offset = appendText(text, offset, line_limit, " ", white);
    offset = appendNumber(
        text,
        offset,
        line_limit,
        values.input.camera_pitch_degrees,
        1,
        hudColor(DebugHudColor::Gold)
    );
    offset = appendText(text, offset, line_limit, " ", white);
    offset = appendNumber(text, offset, line_limit, values.input.camera_roll_degrees, 1, hudColor(DebugHudColor::Rose));
    return true;
}

bool DebugHudState::buildBatch(DebugHudText& text, DebugHudBatch& batch) const {
    if (!formatText(text)) {
        batch = {};
        return false;
    }
    packText(text, batch, TextPlacement{}, {}, {});
    return true;
}

double DebugHudState::defaultNow(void*) noexcept {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

size_t DebugHudState::appendText(
    DebugHudText& text,
    size_t const offset,
    size_t const limit,
    std::string_view const value,
    TextColor const color
) const {
    size_t const bounded_limit = std::min(limit, DEBUG_HUD_MAX_TEXT_BYTES);
    if (offset >= bounded_limit) {
        return bounded_limit;
    }
    size_t const count = std::min(value.size(), bounded_limit - offset);
    if (text.value.size() < offset) {
        text.value.resize(offset, '\0');
    }
    if (text.value.size() > offset) {
        text.value.resize(offset);
    }
    static_cast<void>(::client::appendText(text, value.substr(0U, count), color));
    return offset + count;
}

size_t DebugHudState::appendNumber(
    DebugHudText& text,
    size_t const offset,
    size_t const limit,
    double const value,
    int const decimals,
    TextColor const color
    ) const {
    size_t const bounded_limit = std::min(limit, DEBUG_HUD_MAX_TEXT_BYTES);
    if (offset >= bounded_limit) {
        return bounded_limit;
    }
    std::array<char, 128U> formatted{};
    size_t const count = formatter_.format(
        formatted.data(),
        std::min(formatted.size(), bounded_limit - offset),
        value,
        decimals,
        formatter_.context
    );
    size_t const bounded_count = std::min(count, bounded_limit - offset);
    static_cast<void>(::client::appendText(
        text,
        std::string_view(formatted.data(), bounded_count),
        color
    ));
    return offset + bounded_count;
}

void DebugHudState::recordPresentation(
    double const monotonic_seconds,
    bool const presented
) noexcept {
    if (presented) {
        size_t const write_index = (presented_begin_ + presented_size_) % presented_times_.size();
        if (presented_size_ == presented_times_.size()) {
            presented_times_[presented_begin_] = monotonic_seconds;
            presented_begin_ = (presented_begin_ + 1) % presented_times_.size();
        } else {
            presented_times_[write_index] = monotonic_seconds;
            ++presented_size_;
        }
    }

    while (presented_size_ > 0) {
        double const oldest = presented_times_[presented_begin_];
        if (monotonic_seconds - oldest <= DEBUG_HUD_FPS_WINDOW_SECONDS) {
            break;
        }
        presented_begin_ = (presented_begin_ + 1) % presented_times_.size();
        --presented_size_;
    }
}

} // namespace client
