#include <client/render/DebugHud.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace client {

namespace {

constexpr uint8_t FIRST_GLYPH = 0x20;
constexpr uint8_t LAST_GLYPH = 0x7f;
constexpr uint8_t UNSUPPORTED_GLYPH = '?';
constexpr float MIN_DPI_SCALE = 0.25F;
constexpr float MAX_DPI_SCALE = 8.0F;

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
    if (byte == 0 || (byte >= FIRST_GLYPH && byte <= LAST_GLYPH)) {
        return byte;
    }
    return UNSUPPORTED_GLYPH;
}

uint32_t packDebugHudAscii(
    uint8_t const c0,
    uint8_t const c1,
    uint8_t const c2,
    uint8_t const c3
) noexcept {
    uint32_t const sanitized_c0 = sanitizeDebugHudByte(c0);
    uint32_t const sanitized_c1 = sanitizeDebugHudByte(c1);
    uint32_t const sanitized_c2 = sanitizeDebugHudByte(c2);
    uint32_t const sanitized_c3 = sanitizeDebugHudByte(c3);
    return sanitized_c0
        | (sanitized_c1 << 8)
        | (sanitized_c2 << 16)
        | (sanitized_c3 << 24);
}

void packDebugHudText(DebugHudText const& text, DebugHudBatch& batch) noexcept {
    batch.instances.fill({ });
    batch.size = 0;
    size_t const bounded_size = std::min(text.size, text.bytes.size());
    size_t line = 0;
    size_t column = 0;
    for (size_t offset = 0; offset < bounded_size && line < DEBUG_HUD_LINE_COUNT; ++offset) {
        uint8_t const character = static_cast<uint8_t>(text.bytes[offset]);
        if (character == static_cast<uint8_t>('\n')) {
            ++line;
            column = 0;
            continue;
        }
        if (column >= DEBUG_HUD_MAX_LINE_BYTES) {
            continue;
        }
        size_t const word_index = line * DEBUG_HUD_WORDS_PER_LINE + column / 4;
        uint32_t const character_shift = static_cast<uint32_t>(8 * (column % 4));
        batch.instances[word_index].packed_ascii |=
            static_cast<uint32_t>(sanitizeDebugHudByte(character)) << character_shift;
        batch.size = std::max(batch.size, word_index + 1);
        ++column;
    }
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

bool DebugHudState::formatText(DebugHudText& text) const noexcept {
    text.size = 0;
    if (!enabled_) {
        return false;
    }

    DebugHudSnapshot const values = snapshot();
    size_t offset = 0;
    offset = appendText(text, offset, "FPS:");
    offset = appendNumber(text, offset, values.presented_fps, 1);
    offset = appendText(text, offset, "\nUPTIME:");
    offset = appendNumber(text, offset, values.uptime_seconds, 1);
    offset = appendText(text, offset, "s\nXYZ:");
    offset = appendNumber(text, offset, values.input.player_x, 1);
    offset = appendText(text, offset, ",");
    offset = appendNumber(text, offset, values.input.player_y, 1);
    offset = appendText(text, offset, ",");
    offset = appendNumber(text, offset, values.input.player_z, 1);
    offset = appendText(text, offset, "\nY/P/R(deg):");
    offset = appendNumber(text, offset, values.input.camera_yaw_degrees, 1);
    offset = appendText(text, offset, ",");
    offset = appendNumber(text, offset, values.input.camera_pitch_degrees, 1);
    offset = appendText(text, offset, ",");
    offset = appendNumber(text, offset, values.input.camera_roll_degrees, 1);
    text.size = offset;
    return true;
}

bool DebugHudState::buildBatch(DebugHudText& text, DebugHudBatch& batch) const noexcept {
    if (!formatText(text)) {
        batch.size = 0;
        return false;
    }
    packDebugHudText(text, batch);
    return true;
}

double DebugHudState::defaultNow(void*) noexcept {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

size_t DebugHudState::appendText(
    DebugHudText& text,
    size_t const offset,
    std::string_view const value
) const noexcept {
    size_t const count = std::min(value.size(), text.bytes.size() - std::min(offset, text.bytes.size()));
    if (offset >= text.bytes.size()) {
        return text.bytes.size();
    }
    std::memcpy(text.bytes.data() + offset, value.data(), count);
    return offset + count;
}

size_t DebugHudState::appendNumber(
    DebugHudText& text,
    size_t const offset,
    double const value,
    int const decimals
) const noexcept {
    if (offset >= text.bytes.size()) {
        return text.bytes.size();
    }
    size_t const count = formatter_.format(
        text.bytes.data() + offset,
        text.bytes.size() - offset,
        value,
        decimals,
        formatter_.context
    );
    return std::min(text.bytes.size(), offset + count);
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
