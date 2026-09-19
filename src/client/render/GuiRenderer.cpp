#include <client/render/GuiRenderer.hpp>

#include <algorithm>
#include <utility>

namespace client {

namespace {

[[nodiscard]] TextDocument colorLines(
    TextDocument const& document,
    std::span<TextColor const> const line_colors,
    TextColor const fallback_color
) {
    if (line_colors.empty() || !document.spans.empty()) {
        return document;
    }

    TextDocument colored{};
    size_t line = 0U;
    size_t offset = 0U;
    while (offset < document.value.size()) {
        size_t const newline = document.value.find('\n', offset);
        size_t const end = newline == std::string::npos ? document.value.size() : newline + 1U;
        TextColor const color = line < line_colors.size()
            ? line_colors[line]
            : fallback_color;
        if (!appendText(colored, std::string_view(document.value).substr(offset, end - offset), color)) {
            return {};
        }
        offset = end;
        ++line;
    }
    return colored;
}

} // namespace

void GuiRenderer::begin() noexcept {
    text_renderer_.clear();
    batches_.clear();
}

bool GuiRenderer::submit(
    TextDocument const& document,
    std::span<TextColor const> const line_colors,
    TextColor const fallback_color,
    TextPlacement const placement
) {
    if (placement.space != TextSpace::Screen) {
        return false;
    }
    TextDocument const colored = colorLines(document, line_colors, fallback_color);
    TextBatch batch{};
    packText(colored, batch, placement, {}, fallback_color);
    appendBatch(batch);
    return !batch.glyphs.empty();
}

bool GuiRenderer::submit(TextBatch const& batch) {
    if (batch.space != TextSpace::Screen) {
        return false;
    }
    appendBatch(batch);
    return !batch.glyphs.empty();
}

bool GuiRenderer::submit(
    std::string_view const value,
    TextColor const color
) {
    text_renderer_.clear();
    if (!text_renderer_.submit(value, color)) {
        return false;
    }
    return submit(text_renderer_, glm::mat4{1.0F}, 0U, 0U);
}

bool GuiRenderer::submit(
    TextRenderer const& renderer,
    glm::mat4 const& projection_view,
    uint32_t const width,
    uint32_t const height
) {
    static_cast<void>(projection_view);
    static_cast<void>(width);
    static_cast<void>(height);
    bool submitted = false;
    for (TextLabel const& label : renderer.labels()) {
        if (label.placement.space != TextSpace::Screen) {
            continue;
        }
        TextDocument document{};
        if (!appendText(document, label.value, label.color)) {
            continue;
        }
        TextBatch batch{};
        packText(document, batch, label.placement, {}, label.color);
        submitted = !batch.glyphs.empty() || submitted;
        appendBatch(std::move(batch));
    }
    return submitted;
}

TextBatch const& GuiRenderer::text() const noexcept {
    static TextBatch const EMPTY{};
    return batches_.empty() ? EMPTY : batches_.front();
}

std::span<TextBatch const> GuiRenderer::batches() const noexcept {
    return batches_;
}

bool GuiRenderer::empty() const noexcept {
    return batches_.empty();
}

void GuiRenderer::appendBatch(TextBatch batch) {
    if (!batch.glyphs.empty()) {
        batches_.push_back(std::move(batch));
    }
}

} // namespace client
