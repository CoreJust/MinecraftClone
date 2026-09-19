#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace client {

struct TextColor final {
    float red = 1.0F;
    float green = 1.0F;
    float blue = 1.0F;
    float alpha = 1.0F;

    [[nodiscard]] uint32_t packed() const noexcept;
};

enum class TextSpace : uint8_t {
    Screen,
    World,
};

enum class TextShaderMode : uint32_t {
    Screen = 0U,
    World = 1U,
};

[[nodiscard]]
constexpr TextShaderMode textShaderMode(TextSpace const space) noexcept
{
    return space == TextSpace::Screen
        ? TextShaderMode::Screen
        : TextShaderMode::World;
}

struct TextPlacement final {
    TextSpace space = TextSpace::Screen;
    std::array<float, 3> position{ 0.0F, 0.0F, 0.0F };
    float scale = 1.0F;
    std::array<float, 3> right{ 1.0F, 0.0F, 0.0F };
    std::array<float, 3> up{ 0.0F, 1.0F, 0.0F };
};

struct TextLabel final {
    std::string value;
    TextColor color{};
    TextPlacement placement{};
};

struct TextColorSpan final {
    size_t offset = 0U;
    size_t length = 0U;
    uint32_t packed_color = 0xFFFF'FFFFU;
};

struct TextDocument final {
    std::string value;
    std::vector<TextColorSpan> spans;

    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
};

struct TextLayout final {
    float glyph_width = 16.0F;
    float glyph_height = 32.0F;
    float glyph_advance = 16.0F;
    float line_advance = 40.0F;
    float max_width = 0.0F;
};

struct TextGlyph final {
    uint32_t character = 0U;
    uint32_t packed_color = 0xFFFF'FFFFU;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float padding = 0.0F;
    std::array<float, 3> right{ 0.0F, 0.0F, 0.0F };
    std::array<float, 3> up{ 0.0F, 0.0F, 0.0F };
    float width = 16.0F;
    float height = 32.0F;
};
static_assert(sizeof(TextGlyph) == 56U);

struct TextBatch final {
    std::vector<TextGlyph> glyphs;
    TextSpace space = TextSpace::Screen;
};

[[nodiscard]] uint8_t sanitizeTextByte(uint8_t byte) noexcept;

void clearText(TextDocument& document) noexcept;

[[nodiscard]] bool appendText(
    TextDocument& document,
    std::string_view value,
    TextColor color = {}
);

void packText(
    TextDocument const& document,
    TextBatch& batch,
    TextPlacement placement = {},
    TextLayout layout = {},
    TextColor fallback_color = {}
);

class TextRenderer final {
public:
    void clear() noexcept;

    [[nodiscard]] bool submit(
        std::string_view value,
        TextColor color = {},
        TextPlacement placement = {}
    );

    [[nodiscard]] std::span<TextLabel const> labels() const noexcept;

    [[nodiscard]] bool append(std::string_view value, TextColor color = {});
    [[nodiscard]] bool appendLine(std::string_view value, TextColor color = {});

    [[nodiscard]] TextDocument const& document() const noexcept;
    [[nodiscard]] TextBatch const& batch() const noexcept;

    void build(
        TextColor fallback_color = {},
        TextPlacement placement = {},
        TextLayout layout = {}
    );

private:
    std::vector<TextLabel> labels_{};
    TextDocument document_{};
    TextBatch batch_{};
};

} // namespace client
