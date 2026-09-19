#pragma once

#include <client/render/TextRenderer.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace client {

class GuiRenderer final {
public:
    void begin() noexcept;

    [[nodiscard]] bool submit(
        TextDocument const& document,
        std::span<TextColor const> line_colors = {},
        TextColor fallback_color = {},
        TextPlacement placement = {}
    );
    [[nodiscard]] bool submit(TextBatch const& batch);
    [[nodiscard]] bool submit(std::string_view value, TextColor color = {});
    [[nodiscard]] bool submit(
        TextRenderer const& renderer,
        glm::mat4 const& projection_view,
        uint32_t width,
        uint32_t height
    );

    [[nodiscard]] TextBatch const& text() const noexcept;
    [[nodiscard]] std::span<TextBatch const> batches() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    void appendBatch(TextBatch batch);

    TextRenderer text_renderer_{};
    std::vector<TextBatch> batches_{};
};

} // namespace client
