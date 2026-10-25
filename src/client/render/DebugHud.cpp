#include <client/render/DebugHud.hpp>

#include <client/render/TextRenderer.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

[[nodiscard]] constexpr TextColor colorFromHex(uint32_t const value) noexcept {
    constexpr float channel_scale = 1.0F / 255.0F;
    return {
        static_cast<float>((value >> 16U) & 0xFFU) * channel_scale,
        static_cast<float>((value >> 8U) & 0xFFU) * channel_scale,
        static_cast<float>(value & 0xFFU) * channel_scale,
        1.0F,
    };
}

constexpr TextColor HUD_FOREGROUND = colorFromHex(0xF3'EAD2U);
constexpr TextColor HUD_OUTLINE = colorFromHex(0x14'242DU);
constexpr TextColor HUD_HINT = colorFromHex(0xE5'D29BU);
constexpr TextColor HUD_COORDINATES = colorFromHex(0xFF'7070U);
constexpr TextColor HUD_COORDINATE_Y = colorFromHex(0x75'FF8CU);
constexpr TextColor HUD_COORDINATE_Z = colorFromHex(0x62'BFFFU);
constexpr TextColor HUD_HEADING = colorFromHex(0xE5'C798U);
constexpr TextColor HUD_ACTIVE = colorFromHex(0xB2'D39CU);
constexpr TextColor HUD_INACTIVE = colorFromHex(0xB5'BDB7U);

struct HudFlowEntry final {
    std::string value;
    TextColor color = HUD_FOREGROUND;
};

[[nodiscard]] std::string formatNumber(double const value, int const decimals, bool const available = true) {
    if (!available || !std::isfinite(value)) {
        return "Unavailable";
    }
    std::array<char, 48U> buffer{};
    std::to_chars_result const result = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::fixed,
        decimals
    );
    if (result.ec != std::errc{}) {
        return "Unavailable";
    }
    return std::string{ buffer.data(), result.ptr };
}

[[nodiscard]] std::string formatUptime(double const seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return "Unavailable";
    }
    double const whole_seconds = std::floor(seconds);
    double const minutes = std::floor(whole_seconds / 60.0);
    uint64_t const remainder = static_cast<uint64_t>(std::fmod(whole_seconds, 60.0));
    std::array<char, 48U> buffer{};
    char* cursor = buffer.data();
    if (minutes < 10.0) {
        *cursor++ = '0';
    }
    std::to_chars_result const minute_result = std::to_chars(
        cursor,
        buffer.data() + buffer.size(),
        minutes,
        std::chars_format::fixed,
        0
    );
    if (minute_result.ec != std::errc{}) {
        return "Unavailable";
    }
    cursor = minute_result.ptr;
    *cursor++ = ':';
    if (remainder < 10U) {
        *cursor++ = '0';
    }
    std::to_chars_result const second_result = std::to_chars(cursor, buffer.data() + buffer.size(), remainder);
    if (second_result.ec != std::errc{}) {
        return "Unavailable";
    }
    return std::string{ buffer.data(), second_result.ptr };
}

void submitOutlined(
    TextRenderer& output,
    TextStyle const style,
    std::string_view const value,
    TextColor const color,
    float const x,
    float const y,
    float const scale,
    float const outline_offset
) {
    if (style != TextStyle::Adaptive) {
        for (auto const [offset_x, offset_y] : std::array<std::array<float, 2U>, 4U>{
            std::array<float, 2U>{ -outline_offset, 0.0F },
            std::array<float, 2U>{ outline_offset, 0.0F },
            std::array<float, 2U>{ 0.0F, -outline_offset },
            std::array<float, 2U>{ 0.0F, outline_offset },
        }) {
            static_cast<void>(output.submit(
                value,
                HUD_OUTLINE,
                TextPlacement{
                    .position = { x + offset_x, y + offset_y, 0.0F },
                    .scale = scale,
                    .style = style == TextStyle::ColoredAdaptive ? TextStyle::AdaptiveHalo : TextStyle::Fixed,
                    .light_color = {1.0F, 1.0F, 1.0F, 1.0F},
                    .dark_color = HUD_OUTLINE,
                }
            ));
        }
    }
    static_cast<void>(output.submit(
        value,
        color,
        TextPlacement{
            .position = { x, y, 0.0F },
            .scale = scale,
            .style = style,
            .light_color = color,
            .dark_color = HUD_OUTLINE,
        }
    ));
}

[[nodiscard]] float submitWrapped(
    TextRenderer& output,
    TextStyle const style,
    std::string_view value,
    TextColor const color,
    float const x,
    float y,
    float const width,
    float const scale,
    float const line_advance,
    float const outline_offset
) {
    size_t const columns = static_cast<size_t>((std::max)(
        1.0F,
        std::floor(width / (DEBUG_HUD_GLYPH_WIDTH_PIXELS * scale))
    ));
    float last_bottom = y;
    bool submitted = false;
    while (!value.empty()) {
        size_t count = (std::min)(columns, value.size());
        if (count < value.size()) {
            size_t const space = value.rfind(' ', count);
            if (space != std::string_view::npos && space > 0U) {
                count = space;
            }
        }
        std::string_view const line = value.substr(0U, count);
        submitOutlined(output, style, line, color, x, y, scale, outline_offset);
        last_bottom = y + DEBUG_HUD_GLYPH_HEIGHT_PIXELS * scale;
        submitted = true;
        value.remove_prefix(count);
        while (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1U);
        }
        if (!value.empty()) {
            y += line_advance;
        }
    }
    return submitted ? last_bottom : y;
}

[[nodiscard]] float submitFlow(
    TextRenderer& output,
    TextStyle const style,
    std::span<HudFlowEntry const> entries,
    float const x,
    float const y,
    float const width,
    float const scale,
    float const gap,
    float const line_advance,
    float const outline_offset
) {
    float cursor_x = x;
    float cursor_y = y;
    float bottom = y;
    float const max_x = x + width;
    for (HudFlowEntry const& entry : entries) {
        float const entry_width = static_cast<float>(entry.value.size())
            * DEBUG_HUD_GLYPH_WIDTH_PIXELS * scale;
        if (cursor_x > x && cursor_x + entry_width > max_x) {
            cursor_x = x;
            cursor_y = (std::max)(cursor_y + line_advance,
                bottom + line_advance - DEBUG_HUD_GLYPH_HEIGHT_PIXELS * scale);
        }
        bottom = (std::max)(bottom, submitWrapped(
            output,
            style,
            entry.value,
            entry.color,
            cursor_x,
            cursor_y,
            max_x - cursor_x,
            scale,
            line_advance,
            outline_offset
        ));
        cursor_x += entry_width + gap;
    }
    return bottom;
}

[[nodiscard]] std::string makeFieldRow(
    std::string_view const label,
    std::string_view const value,
    size_t const label_width
) {
    std::string result{ label };
    result.append(label_width > label.size() ? label_width - label.size() : 0U, ' ');
    result.push_back(' ');
    result.append(value);
    return result;
}

[[nodiscard]] float submitFields(
    TextRenderer& output,
    TextStyle const style,
    std::span<HudFlowEntry const> fields,
    float const x,
    float y,
    float const width,
    float const scale,
    float const line_advance,
    float const outline_offset,
    float const dpi_scale
) {
    float bottom = y;
    for (HudFlowEntry const& field : fields) {
        bottom = submitWrapped(
            output,
            style,
            field.value,
            field.color,
            x,
            y,
            width,
            scale,
            line_advance,
            outline_offset
        );
        y = bottom + 4.0F * dpi_scale;
    }
    return bottom;
}

[[nodiscard]] float submitSingleRow(
    TextRenderer& output, TextStyle const style, std::span<HudFlowEntry const> const fields,
    float const x, float const y, float const width, float const scale,
    float const line_advance, float const outline_offset
) {
    size_t characters = 2U * (fields.size() - 1U);
    for (auto const& field : fields) { characters += field.value.size(); }
    float const fitted_scale = (std::min)(scale,
        width / (static_cast<float>(characters) * DEBUG_HUD_GLYPH_WIDTH_PIXELS));
    float const row_scale = fitted_scale >= scale * 0.75F ? fitted_scale : scale;
    return submitFlow(output, style, fields, x, y, width, row_scale,
        2.0F * DEBUG_HUD_GLYPH_WIDTH_PIXELS * row_scale, line_advance, outline_offset);
}

[[nodiscard]] float submitCoordinates(
    TextRenderer& output, TextStyle const style, DebugHudInput const& input,
    float const x, float const y, float const width, float const scale,
    float const line_advance, float const outline_offset
) {
    std::array<HudFlowEntry, 3U> const axes{
        HudFlowEntry{"X " + formatNumber(input.player_x, 1), HUD_COORDINATES},
        HudFlowEntry{"Y " + formatNumber(input.player_z, 1), HUD_COORDINATE_Y},
        HudFlowEntry{"Z " + formatNumber(input.player_y, 1), HUD_COORDINATE_Z},
    };
    float const row_bottom = submitSingleRow(
        output, style, axes, x, y, width, scale, line_advance, outline_offset
    );
    std::string const orientation = "YPR " + formatNumber(input.camera_yaw_degrees, 1)
        + " / " + formatNumber(input.camera_pitch_degrees, 1)
        + " / " + formatNumber(input.camera_roll_degrees, 1);
    return submitWrapped(output, style, orientation, HUD_FOREGROUND, x,
        (std::max)(y + line_advance, row_bottom + line_advance - DEBUG_HUD_GLYPH_HEIGHT_PIXELS * scale),
        width, scale, line_advance, outline_offset);
}

[[nodiscard]] float submitMovement(
    TextRenderer& output, TextStyle const style, DebugHudInput const& input,
    float const x, float const y, float const width, float const scale,
    float const line_advance, float const outline_offset
) {
    std::array<HudFlowEntry, 3U> const speed{
        HudFlowEntry{"Speed " + formatNumber(input.speedup, 0) + "x", HUD_FOREGROUND},
        HudFlowEntry{"Selected " + formatNumber(input.selected_speedup, 0) + "x", HUD_FOREGROUND},
        HudFlowEntry{std::string{"Accel "} + (input.acceleration_enabled ? "ON" : "OFF"), HUD_FOREGROUND},
    };
    float const speed_bottom = submitSingleRow(
        output, style, speed, x, y, width, scale, line_advance, outline_offset
    );
    std::array<HudFlowEntry, 2U> const mobility{
        HudFlowEntry{std::string{"Bypass "} + (input.collision_bypass_enabled ? "ON" : "OFF"),
            input.collision_bypass_enabled ? HUD_ACTIVE : HUD_INACTIVE},
        HudFlowEntry{std::string{"Flight "} + (input.flight_enabled ? "ON" : "OFF"),
            input.flight_enabled ? HUD_ACTIVE : HUD_INACTIVE},
    };
    return submitSingleRow(output, style, mobility, x,
        (std::max)(y + line_advance, speed_bottom + line_advance - DEBUG_HUD_GLYPH_HEIGHT_PIXELS * scale),
        width, scale, line_advance, outline_offset);
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

DebugHudLayout layoutDebugHud(
    uint32_t const framebuffer_width,
    uint32_t const framebuffer_height,
    float const requested_dpi_scale,
    bool const force_compact
) noexcept {
    float const dpi_scale = std::isfinite(requested_dpi_scale) && requested_dpi_scale > 0.0F
        ? std::clamp(requested_dpi_scale, MIN_DPI_SCALE, MAX_DPI_SCALE)
        : 1.0F;
    float const logical_width = static_cast<float>(framebuffer_width) / dpi_scale;
    float const logical_height = static_cast<float>(framebuffer_height) / dpi_scale;
    bool const compact = force_compact || logical_width < 1050.0F || logical_height < 520.0F;
    float const logical_margin = compact ? 12.0F : 24.0F;
    float const logical_gap = compact ? 24.0F : 40.0F;
    float const logical_map_size = (std::max)(
        128.0F,
        std::floor((std::min)({ 192.0F, logical_height * 0.35F, logical_width * 0.19F }) / 16.0F)
            * 16.0F
    );
    float const map_x = std::floor(logical_width - logical_map_size - 34.0F);
    float constexpr map_y = 44.0F;
    DebugHudLayout layout{
        .compact = compact,
        .dpi_scale = dpi_scale,
        .framebuffer_width_pixels = static_cast<float>(framebuffer_width),
        .framebuffer_height_pixels = static_cast<float>(framebuffer_height),
        .glyph_scale = (compact ? 0.5F : 1.0F) * dpi_scale,
        .small_glyph_scale = 0.5F * dpi_scale,
        .outline_offset_pixels = dpi_scale,
        .margin_pixels = logical_margin * dpi_scale,
        .gap_pixels = logical_gap * dpi_scale,
        .selected_block_y = (logical_height - (compact ? 80.0F : 128.0F)) * dpi_scale,
        .controls_y = (logical_height - (compact ? 40.0F : 48.0F)) * dpi_scale,
        .control_line_advance_pixels = 20.0F * dpi_scale,
        .minimap = {
            .x = map_x * dpi_scale,
            .y = map_y * dpi_scale,
            .width = logical_map_size * dpi_scale,
            .height = logical_map_size * dpi_scale,
        },
        .minimap_interactive = {
            .x = (map_x - 26.0F) * dpi_scale,
            .y = (map_y - 38.0F) * dpi_scale,
            .width = (logical_map_size + 52.0F) * dpi_scale,
            .height = (logical_map_size + 76.0F) * dpi_scale,
        },
    };

    float const map_left = map_x - 26.0F - (compact ? 16.0F : 24.0F);
    float const available_width = (std::max)(
        0.0F,
        (std::min)(logical_width - logical_margin, map_left) - logical_margin
    );
    float const column_width = std::floor((std::max)(0.0F, available_width - logical_gap) * 0.5F);
    layout.left_column_x = logical_margin * dpi_scale;
    layout.right_column_x = (logical_margin + column_width + logical_gap) * dpi_scale;
    layout.column_width_pixels = column_width * dpi_scale;

    float const center_x = layout.minimap.x + layout.minimap.width * 0.5F;
    float const center_y = layout.minimap.y + layout.minimap.height * 0.5F;
    layout.north = {
        .x = center_x - 8.0F * dpi_scale,
        .y = layout.minimap.y - 36.0F * dpi_scale,
    };
    layout.east = {
        .x = layout.minimap.x + layout.minimap.width + 8.0F * dpi_scale,
        .y = center_y - 16.0F * dpi_scale,
    };
    layout.south = {
        .x = center_x - 8.0F * dpi_scale,
        .y = layout.minimap.y + layout.minimap.height + 4.0F * dpi_scale,
    };
    layout.west = {
        .x = layout.minimap.x - 24.0F * dpi_scale,
        .y = center_y - 16.0F * dpi_scale,
    };
    layout.heading = {
        .x = layout.minimap.x + layout.minimap.width - 48.0F * dpi_scale,
        .y = layout.minimap.y - 28.0F * dpi_scale,
    };
    return layout;
}

void buildDebugHudPresentation(
    DebugHudSnapshot const& snapshot,
    DebugHudLayout const& layout,
    TextRenderer& output
) {
    TextStyle const style = snapshot.input.text_style;
    output.clear();
    if (!snapshot.input.gameplay_ui) {
        return;
    }
    float const dpi_scale = layout.dpi_scale;
    float const body_scale = layout.glyph_scale;
    float const small_scale = layout.small_glyph_scale;
    float const outline_offset = layout.outline_offset_pixels;
    float const body_line_advance = (layout.compact ? 20.0F : 36.0F) * dpi_scale;
    float const small_line_advance = layout.control_line_advance_pixels;

    if (snapshot.enabled) {
        float const logical_margin = layout.margin_pixels / dpi_scale;
        float const logical_width = layout.framebuffer_width_pixels / dpi_scale;
        float const map_left = layout.minimap_interactive.x / dpi_scale
            - (layout.compact ? 16.0F : 24.0F);
        float const available_width = (std::max)(
            0.0F,
            (std::min)(logical_width - logical_margin, map_left) - logical_margin
        ) * dpi_scale;
        float const session_y = (layout.compact ? 8.0F : 12.0F) * dpi_scale;
        std::array<HudFlowEntry, 5U> const session{
            HudFlowEntry{ "FPS " + formatNumber(snapshot.presented_fps, 0), HUD_FOREGROUND },
            HudFlowEntry{
                std::string{ layout.compact ? "Up " : "Uptime " }
                    + formatUptime(snapshot.uptime_seconds),
                HUD_FOREGROUND,
            },
            HudFlowEntry{
                "Chunks resident " + (snapshot.input.loaded_dense_chunks_available
                    ? formatNumber(static_cast<double>(snapshot.input.loaded_dense_chunks), 0)
                        + (snapshot.input.resident_chunk_capacity > 0U
                            ? "/" + formatNumber(static_cast<double>(snapshot.input.resident_chunk_capacity), 0)
                            : "")
                    : "--"),
                HUD_FOREGROUND,
            },
            HudFlowEntry{
                "Terrain ready " + (snapshot.input.required_terrain_columns > 0U
                    ? formatNumber(snapshot.input.ready_terrain_columns, 0) + "/"
                        + formatNumber(snapshot.input.required_terrain_columns, 0)
                    : "--"),
                HUD_FOREGROUND,
            },
            HudFlowEntry{
                std::string{ layout.compact ? "RSS " : "Memory " }
                    + (snapshot.input.client_memory_rss_bytes.has_value()
                        ? formatNumber(
                            static_cast<double>(*snapshot.input.client_memory_rss_bytes) / (1024.0 * 1024.0),
                            0
                        ) + " MiB"
                        : "-- MiB"),
                HUD_FOREGROUND,
            },
        };
        float const session_bottom = submitFlow(
            output,
            style,
            session,
            logical_margin * dpi_scale,
            session_y,
            available_width,
            body_scale,
            (layout.compact ? 16.0F : 32.0F) * dpi_scale,
            body_line_advance,
            outline_offset
        );
        if (layout.compact) {
            float const heading_y = session_bottom + 8.0F * dpi_scale;
            float const readout_y = heading_y + 16.0F * dpi_scale;
            submitOutlined(
                output,
                style,
                "Position",
                HUD_HEADING,
                layout.left_column_x,
                heading_y,
                body_scale,
                outline_offset
            );
            submitOutlined(
                output,
                style,
                "Movement",
                HUD_HEADING,
                layout.right_column_x,
                heading_y,
                body_scale,
                outline_offset
            );

            float const position_bottom = submitCoordinates(
                output, style, snapshot.input, layout.left_column_x, readout_y,
                layout.column_width_pixels, body_scale, body_line_advance, outline_offset
            );

            float const movement_bottom = submitMovement(
                output, style, snapshot.input, layout.right_column_x, readout_y,
                layout.column_width_pixels, body_scale, body_line_advance, outline_offset
            );

            float const climate_y = (std::max)(position_bottom, movement_bottom) + 12.0F * dpi_scale;
            std::string const temperature = snapshot.input.temperature_celsius.has_value()
                ? formatNumber(*snapshot.input.temperature_celsius, 1) + " C"
                : "unavailable";
            submitOutlined(
                output,
                style,
                "Temp " + temperature,
                snapshot.input.temperature_celsius.has_value() ? HUD_FOREGROUND : HUD_INACTIVE,
                layout.left_column_x,
                climate_y,
                body_scale,
                outline_offset
            );
        } else {
            float const heading_y = session_bottom + (layout.compact ? 12.0F : 24.0F) * dpi_scale;
            float const readout_y = heading_y + (layout.compact ? 24.0F : 44.0F) * dpi_scale;
            submitOutlined(
                output,
                style,
                "Position",
                HUD_HEADING,
                layout.left_column_x,
                heading_y,
                body_scale,
                outline_offset
            );
            submitOutlined(
                output,
                style,
                "Movement",
                HUD_HEADING,
                layout.right_column_x,
                heading_y,
                body_scale,
                outline_offset
            );

            float const position_bottom = submitCoordinates(
                output, style, snapshot.input, layout.left_column_x, readout_y,
                layout.column_width_pixels, body_scale, body_line_advance, outline_offset
            );

            float const movement_bottom = submitMovement(
                output, style, snapshot.input, layout.right_column_x, readout_y,
                layout.column_width_pixels, body_scale, body_line_advance, outline_offset
            );

            float const climate_y = (std::max)(position_bottom, movement_bottom)
                + (layout.compact ? 16.0F : 24.0F) * dpi_scale;
            submitOutlined(
                output,
                style,
                "Local climate",
                HUD_HEADING,
                layout.left_column_x,
                climate_y,
                body_scale,
                outline_offset
            );
            std::array<HudFlowEntry, 1U> const climate{
                HudFlowEntry{
                    makeFieldRow(
                        "Temperature",
                        formatNumber(
                            snapshot.input.temperature_celsius.value_or(0.0F),
                            1,
                            snapshot.input.temperature_celsius.has_value()
                        ) + " C",
                        11U
                    ),
                    snapshot.input.temperature_celsius.has_value() ? HUD_FOREGROUND : HUD_INACTIVE,
                },
            };
            static_cast<void>(submitFields(
                output,
                style,
                climate,
                layout.left_column_x,
                climate_y + (layout.compact ? 24.0F : 44.0F) * dpi_scale,
                layout.column_width_pixels,
                body_scale,
                body_line_advance,
                outline_offset,
                dpi_scale
            ));
        }
    } else {
        float const movement_y = layout.selected_block_y
            - (layout.compact ? 32.0F : 48.0F) * dpi_scale;
        std::array<HudFlowEntry, 2U> const movement{
            HudFlowEntry{
                std::string{ "Flight " } + (snapshot.input.flight_enabled ? "ON" : "OFF"),
                snapshot.input.flight_enabled ? HUD_ACTIVE : HUD_INACTIVE,
            },
            HudFlowEntry{
                std::string{ "Collision bypass " }
                    + (snapshot.input.collision_bypass_enabled ? "ON" : "OFF"),
                snapshot.input.collision_bypass_enabled ? HUD_ACTIVE : HUD_INACTIVE,
            },
        };
        float const logical_width = layout.framebuffer_width_pixels / dpi_scale;
        static_cast<void>(submitFlow(
            output,
            style,
            movement,
            layout.margin_pixels,
            movement_y,
            (logical_width * dpi_scale) - 2.0F * layout.margin_pixels,
            body_scale,
            (layout.compact ? 16.0F : 32.0F) * dpi_scale,
            body_line_advance,
            outline_offset
        ));
    }

#if defined(__APPLE__)
    std::array<HudFlowEntry, 4U> const primary_hints{
        HudFlowEntry{ "M World map", HUD_HINT },
        HudFlowEntry{ "Esc Leave game", HUD_HINT },
        HudFlowEntry{ "F2 Copy screenshot", HUD_HINT },
        HudFlowEntry{ style == TextStyle::Adaptive ? "F7 Colored HUD" : "F7 Adaptive HUD", HUD_HINT },
    };
#else
    std::array<HudFlowEntry, 3U> const primary_hints{
        HudFlowEntry{ "M World map", HUD_HINT },
        HudFlowEntry{ "Esc Leave game", HUD_HINT },
        HudFlowEntry{ style == TextStyle::Adaptive ? "F7 Colored HUD" : "F7 Adaptive HUD", HUD_HINT },
    };
#endif
    float const logical_width = layout.framebuffer_width_pixels / dpi_scale;
    float const hint_width = (logical_width * dpi_scale) - 2.0F * layout.margin_pixels;
    static_cast<void>(submitFlow(
        output,
        style,
        primary_hints,
        layout.margin_pixels,
        layout.controls_y,
        hint_width,
        small_scale,
        (layout.compact ? 16.0F : 32.0F) * dpi_scale,
        small_line_advance,
        outline_offset
    ));
    std::array<HudFlowEntry, 5U> const secondary_hints{
        HudFlowEntry{ snapshot.enabled ? "F1 Hide HUD" : "F1 Show HUD", HUD_HINT },
        HudFlowEntry{ layout.compact ? "F5/5 View" : "F5/5 Change view", HUD_HINT },
        HudFlowEntry{
            layout.compact
                ? (snapshot.input.touch_flight_help ? "F6/6 Up/down" : "F6/6 Movement")
                : (snapshot.input.touch_flight_help ? "F6/6 Touch up/down" : "F6/6 Cycle movement"),
            HUD_HINT,
        },
        HudFlowEntry{ layout.compact ? "+/- Speed" : "+/- Select speed", HUD_HINT },
        HudFlowEntry{ layout.compact ? "Ctrl Accel" : "Ctrl Toggle acceleration", HUD_HINT },
    };
    static_cast<void>(submitFlow(
        output,
        style,
        secondary_hints,
        layout.margin_pixels,
        layout.controls_y + small_line_advance,
        hint_width,
        small_scale,
        (layout.compact ? 16.0F : 32.0F) * dpi_scale,
        small_line_advance,
        outline_offset
    ));
}

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
    recordPresentation(monotonic_now, input.completed_frame, input.completed_frame_observed_at);
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
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "FPS:", hudColor(line_colors_[0]));
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.presented_fps,
        1,
        hudColor(line_colors_[0])
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, " UPTIME:", hudColor(line_colors_[0]));
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.uptime_seconds,
        1,
        hudColor(line_colors_[0])
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "s", hudColor(line_colors_[0]));
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", hudColor(line_colors_[0]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[1];
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "SPEED:", hudColor(line_colors_[1]));
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.speedup,
        0,
        hudColor(line_colors_[1])
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "x (", hudColor(line_colors_[1]));
    if (values.input.acceleration_enabled) {
        offset = appendText(
            text,
            { .offset = offset, .limit = line_limit },
            "acceleration ON)",
            hudColor(line_colors_[1])
        );
    } else {
        offset = appendNumber(
            text,
            { .offset = offset, .limit = line_limit },
            values.input.selected_speedup,
            0,
            hudColor(line_colors_[1])
        );
        offset = appendText(text, { .offset = offset, .limit = line_limit }, "x selected)", hudColor(line_colors_[1]));
    }
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", hudColor(line_colors_[1]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[2];
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "CAMERA: F5 or 5", hudColor(line_colors_[2]));
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", hudColor(line_colors_[2]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[3];
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "FLIGHT: ", hudColor(line_colors_[3]));
    offset = appendText(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.flight_enabled ? "ON" : "OFF",
        hudColor(line_colors_[3])
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, " BYPASS: ", hudColor(line_colors_[3]));
    offset = appendText(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.collision_bypass_enabled ? "ON" : "OFF",
        hudColor(line_colors_[3])
    );
    if (values.input.touch_flight_help) {
        offset = appendText(text, { .offset = offset, .limit = line_limit }, " TOUCH: UP/DOWN", hudColor(line_colors_[3]));
    } else {
        offset = appendText(text, { .offset = offset, .limit = line_limit }, " (F6 or 6)", hudColor(line_colors_[3]));
    }
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", hudColor(line_colors_[3]));

    line_limit = offset + DEBUG_HUD_LINE_BYTES[4];
    TextColor const white = hudColor(DebugHudColor::White);
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "XYZ: ", white);
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.player_x,
        1,
        hudColor(DebugHudColor::Cyan)
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, " ", white);
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.player_z,
        1,
        hudColor(DebugHudColor::Gold)
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, " ", white);
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.player_y,
        1,
        hudColor(DebugHudColor::Green)
    );
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", white);

    line_limit = offset + DEBUG_HUD_LINE_BYTES[5];
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "YPR deg: ", white);
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.camera_yaw_degrees,
        1,
        hudColor(DebugHudColor::Cyan)
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, " ", white);
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.camera_pitch_degrees,
        1,
        hudColor(DebugHudColor::Gold)
    );
    offset = appendText(text, { .offset = offset, .limit = line_limit }, " ", white);
    offset = appendNumber(
        text,
        { .offset = offset, .limit = line_limit },
        values.input.camera_roll_degrees,
        1,
        hudColor(DebugHudColor::Rose)
    );
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", white);

    line_limit = offset + DEBUG_HUD_LINE_BYTES[6];
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "DENSE CHUNKS: ", white);
    if (values.input.loaded_dense_chunks_available) {
        offset = appendNumber(
            text,
            { .offset = offset, .limit = line_limit },
            static_cast<double>(values.input.loaded_dense_chunks),
            0,
            white
        );
    } else {
        offset = appendText(text, { .offset = offset, .limit = line_limit }, "--", white);
    }
    offset = appendText(text, { .offset = offset, .limit = text.size() + 1U }, "\n", white);

    line_limit = offset + DEBUG_HUD_LINE_BYTES[7];
    offset = appendText(text, { .offset = offset, .limit = line_limit }, "CLIENT MEMORY (RSS): ", white);
    if (values.input.client_memory_rss_bytes.has_value()) {
        offset = appendNumber(
            text,
            { .offset = offset, .limit = line_limit },
            static_cast<double>(*values.input.client_memory_rss_bytes) / (1024.0 * 1024.0),
            1,
            white
        );
        offset = appendText(text, { .offset = offset, .limit = line_limit }, " MiB", white);
    } else {
        offset = appendText(text, { .offset = offset, .limit = line_limit }, "--", white);
    }
    static_cast<void>(appendText(text, { .offset = offset, .limit = line_limit }, "  CLIMATE: --", white));
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
    AppendRange const range,
    std::string_view const value,
    TextColor const color
) const {
    size_t const bounded_limit = std::min(range.limit, DEBUG_HUD_MAX_TEXT_BYTES);
    size_t const offset = range.offset;
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
    AppendRange const range,
    double const value,
    int const decimals,
    TextColor const color
    ) const {
    size_t const bounded_limit = std::min(range.limit, DEBUG_HUD_MAX_TEXT_BYTES);
    size_t const offset = range.offset;
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
    std::optional<RendererFrameIdentity> const& completed_frame,
    std::optional<std::chrono::steady_clock::time_point> const& observed_at
) noexcept {
    bool is_new_completion = completed_frame.has_value() && observed_at.has_value();
    if (is_new_completion && last_completed_frame_.has_value()) {
        is_new_completion = completed_frame->sequence != last_completed_frame_->sequence
            || completed_frame->swapchain_generation != last_completed_frame_->swapchain_generation;
    }
    double const completion_seconds = observed_at.has_value()
        ? std::chrono::duration<double>(observed_at->time_since_epoch()).count() : 0.0;
    is_new_completion = is_new_completion && completion_seconds <= monotonic_seconds
        && monotonic_seconds - completion_seconds <= DEBUG_HUD_FPS_WINDOW_SECONDS;
    if (is_new_completion && presented_size_ != 0U) {
        size_t const last_index = (presented_begin_ + presented_size_ - 1U) % presented_times_.size();
        is_new_completion = completion_seconds >= presented_times_[last_index];
    }
    if (is_new_completion) {
        last_completed_frame_ = completed_frame;
        size_t const write_index = (presented_begin_ + presented_size_) % presented_times_.size();
        if (presented_size_ == presented_times_.size()) {
            presented_times_[presented_begin_] = completion_seconds;
            presented_begin_ = (presented_begin_ + 1) % presented_times_.size();
        } else {
            presented_times_[write_index] = completion_seconds;
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
