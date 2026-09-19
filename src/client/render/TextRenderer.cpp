#include <client/render/TextRenderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace client {
namespace {

constexpr uint8_t FIRST_GLYPH = 0x20U;
constexpr uint8_t LAST_GLYPH = 0x7FU;
constexpr uint8_t UNSUPPORTED_GLYPH = '?';

[[nodiscard]] uint8_t colorByte(float const value) noexcept {
    float const bounded = std::clamp(std::isfinite(value) ? value : 0.0F, 0.0F, 1.0F);
    return static_cast<uint8_t>(std::lround(bounded * 255.0F));
}

[[nodiscard]] uint32_t colorAt(
    TextDocument const& document,
    size_t const offset,
    uint32_t const fallback_color
) noexcept {
    for (TextColorSpan const& span : document.spans) {
        if (offset < span.offset) {
            break;
        }
        if (offset - span.offset < span.length) {
            return span.packed_color;
        }
    }
    return fallback_color;
}

[[nodiscard]] float finiteNonNegative(float const value, float const fallback) noexcept {
    return std::isfinite(value) && value >= 0.0F ? value : fallback;
}

[[nodiscard]] float vectorComponent(
    std::array<float, 3> const& vector,
    size_t const component
) noexcept {
    return std::isfinite(vector[component]) ? vector[component] : 0.0F;
}

[[nodiscard]] size_t utf8SequenceLength(std::string_view const value, size_t const offset) noexcept {
    uint8_t const lead = static_cast<uint8_t>(value[offset]);
    size_t length = 1U;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4U;
    }
    if (offset + length > value.size()) {
        return 1U;
    }
    for (size_t index = 1U; index < length; ++index) {
        if ((static_cast<uint8_t>(value[offset + index]) & 0xC0U) != 0x80U) {
            return 1U;
        }
    }
    return length;
}

} // namespace

uint32_t TextColor::packed() const noexcept {
    return static_cast<uint32_t>(colorByte(red))
        | (static_cast<uint32_t>(colorByte(green)) << 8U)
        | (static_cast<uint32_t>(colorByte(blue)) << 16U)
        | (static_cast<uint32_t>(colorByte(alpha)) << 24U);
}

uint8_t sanitizeTextByte(uint8_t const byte) noexcept {
    if (byte == 0U || (byte >= FIRST_GLYPH && byte <= LAST_GLYPH)) {
        return byte;
    }
    return UNSUPPORTED_GLYPH;
}

size_t TextDocument::size() const noexcept {
    return value.size();
}

bool TextDocument::empty() const noexcept {
    return value.empty();
}

void clearText(TextDocument& document) noexcept {
    document.value.clear();
    document.spans.clear();
}

bool appendText(
    TextDocument& document,
    std::string_view const value,
    TextColor const color
) {
    if (value.empty()) {
        return true;
    }
    uint32_t const packed_color = color.packed();
    size_t const offset = document.value.size();
    try {
        document.value.append(value);
        if (!document.spans.empty()
            && document.spans.back().offset + document.spans.back().length == offset
            && document.spans.back().packed_color == packed_color) {
            document.spans.back().length += value.size();
        } else {
            document.spans.push_back({
                .offset = offset,
                .length = value.size(),
                .packed_color = packed_color,
            });
        }
    } catch (...) {
        document.value.resize(offset);
        return false;
    }
    return true;
}

void packText(
    TextDocument const& document,
    TextBatch& batch,
    TextPlacement const placement,
    TextLayout const layout,
    TextColor const fallback_color
) {
    batch.glyphs.clear();
    batch.space = placement.space;
    if (document.empty()) {
        return;
    }

    float const advance = finiteNonNegative(layout.glyph_advance, 16.0F);
    float const line_advance = finiteNonNegative(layout.line_advance, 40.0F);
    float const glyph_width = finiteNonNegative(layout.glyph_width, 16.0F);
    float const glyph_height = finiteNonNegative(layout.glyph_height, 32.0F);
    float const max_width = finiteNonNegative(layout.max_width, 0.0F);
    float const scale = finiteNonNegative(placement.scale, 1.0F);
    uint32_t const fallback = fallback_color.packed();
    batch.glyphs.reserve(document.size());

    float column = 0.0F;
    float line = 0.0F;
    for (size_t offset = 0U; offset < document.value.size();) {
        uint8_t const character = static_cast<uint8_t>(document.value[offset]);
        size_t const next_offset = offset + utf8SequenceLength(document.value, offset);
        if (character == static_cast<uint8_t>('\n')) {
            column = 0.0F;
            line += 1.0F;
            offset = next_offset;
            continue;
        }
        if (max_width > 0.0F && column > 0.0F && column * advance + advance > max_width) {
            column = 0.0F;
            line += 1.0F;
        }

        float const local_x = column * advance * scale;
        float const local_y = line * line_advance * scale;
        TextGlyph glyph{
            .character = static_cast<uint32_t>(sanitizeTextByte(character)),
            .packed_color = colorAt(document, offset, fallback),
        };
        if (placement.space == TextSpace::Screen) {
            glyph.width = glyph_width * scale;
            glyph.height = glyph_height * scale;
            glyph.x = vectorComponent(placement.position, 0U) + local_x;
            glyph.y = vectorComponent(placement.position, 1U) + local_y;
            glyph.z = 0.0F;
        } else {
            glyph.width = glyph_width;
            glyph.height = glyph_height;
            for (size_t axis = 0U; axis < 3U; ++axis) {
                glyph.right[axis] = vectorComponent(placement.right, axis) * scale;
                glyph.up[axis] = vectorComponent(placement.up, axis) * scale;
            }
            glyph.x = vectorComponent(placement.position, 0U)
                + vectorComponent(placement.right, 0U) * local_x
                + vectorComponent(placement.up, 0U) * local_y;
            glyph.y = vectorComponent(placement.position, 1U)
                + vectorComponent(placement.right, 1U) * local_x
                + vectorComponent(placement.up, 1U) * local_y;
            glyph.z = vectorComponent(placement.position, 2U)
                + vectorComponent(placement.right, 2U) * local_x
                + vectorComponent(placement.up, 2U) * local_y;
        }
        batch.glyphs.push_back(glyph);
        column += 1.0F;
        offset = next_offset;
    }
}

void TextRenderer::clear() noexcept {
    labels_.clear();
    clearText(document_);
    batch_ = {};
}

bool TextRenderer::submit(
    std::string_view const value,
    TextColor const color,
    TextPlacement const placement
) {
    try {
        labels_.push_back({
            .value = std::string(value),
            .color = color,
            .placement = placement,
        });
    } catch (...) {
        return false;
    }
    return true;
}

std::span<TextLabel const> TextRenderer::labels() const noexcept {
    return labels_;
}

bool TextRenderer::append(
    std::string_view const value,
    TextColor const color
) {
    return appendText(document_, value, color);
}

bool TextRenderer::appendLine(
    std::string_view const value,
    TextColor const color
) {
    return appendText(document_, value, color) && appendText(document_, "\n", color);
}

TextDocument const& TextRenderer::document() const noexcept {
    return document_;
}

TextBatch const& TextRenderer::batch() const noexcept {
    return batch_;
}

void TextRenderer::build(
    TextColor const fallback_color,
    TextPlacement const placement,
    TextLayout const layout
) {
    packText(document_, batch_, placement, layout, fallback_color);
}

} // namespace client
