#include <client/render/TextRenderer.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

TEST(TextRendererTest, PacksArbitraryLengthTextWithoutHudLimit) {
    static constexpr uint32_t CHARACTER_COUNT = 512U;
    client::TextDocument document;
    std::string value;
    value.reserve(CHARACTER_COUNT);
    for (uint32_t index = 0U; index < CHARACTER_COUNT; ++index) {
        value.push_back(static_cast<char>('A' + index % 26U));
    }

    ASSERT_TRUE(client::appendText(document, value));
    client::TextBatch batch;
    client::packText(document, batch);

    ASSERT_EQ(document.size(), value.size());
    ASSERT_EQ(batch.glyphs.size(), value.size());
    EXPECT_EQ(batch.glyphs.front().character, static_cast<uint32_t>('A'));
    EXPECT_EQ(batch.glyphs.back().character, static_cast<uint32_t>('A' + (CHARACTER_COUNT - 1U) % 26U));
}

TEST(TextRendererTest, PreservesColorSpansPerGlyph) {
    client::TextDocument document;
    client::TextColor const red{1.0F, 0.0F, 0.0F, 1.0F};
    client::TextColor const blue{0.0F, 0.0F, 1.0F, 1.0F};
    ASSERT_TRUE(client::appendText(document, "AB", red));
    ASSERT_TRUE(client::appendText(document, "CD", blue));

    client::TextBatch batch;
    client::packText(document, batch);

    ASSERT_EQ(batch.glyphs.size(), 4U);
    EXPECT_EQ(batch.glyphs[0].packed_color, red.packed());
    EXPECT_EQ(batch.glyphs[1].packed_color, red.packed());
    EXPECT_EQ(batch.glyphs[2].packed_color, blue.packed());
    EXPECT_EQ(batch.glyphs[3].packed_color, blue.packed());
}

TEST(TextRendererTest, LaysOutNewlinesAndWrapsDynamically) {
    client::TextDocument document;
    ASSERT_TRUE(client::appendText(document, "ABCDE\nFGHIJ"));
    client::TextBatch batch;
    client::TextLayout const layout{
        .glyph_width = 1.0F,
        .glyph_height = 1.0F,
        .glyph_advance = 10.0F,
        .line_advance = 20.0F,
        .max_width = 30.0F,
    };
    client::packText(document, batch, {}, layout);

    ASSERT_EQ(batch.glyphs.size(), 10U);
    EXPECT_FLOAT_EQ(batch.glyphs[0].x, 0.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[2].x, 20.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[3].x, 0.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[3].y, 20.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[5].x, 0.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[5].y, 40.0F);
}

TEST(TextRendererTest, BakesWorldPlacementOrientationAndScale) {
    client::TextDocument document;
    ASSERT_TRUE(client::appendText(document, "AB"));
    client::TextPlacement const placement{
        .space = client::TextSpace::World,
        .position = {10.0F, 20.0F, 30.0F},
        .scale = 0.5F,
        .right = {0.0F, 1.0F, 0.0F},
        .up = {0.0F, 0.0F, 1.0F},
    };
    client::TextBatch batch;
    client::packText(document, batch, placement);

    ASSERT_EQ(batch.space, client::TextSpace::World);
    ASSERT_EQ(batch.glyphs.size(), 2U);
    EXPECT_FLOAT_EQ(batch.glyphs[0].x, 10.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[0].y, 20.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[0].z, 30.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[1].x, 10.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[1].y, 28.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[1].z, 30.0F);
    EXPECT_EQ(batch.glyphs[0].right, (std::array<float, 3>{ 0.0F, 0.5F, 0.0F }));
    EXPECT_EQ(batch.glyphs[0].up, (std::array<float, 3>{ 0.0F, 0.0F, 0.5F }));
}

TEST(TextRendererTest, RetainsColoredWorldLabelAndPlacement) {
    client::TextRenderer renderer;
    client::TextPlacement const placement{
        .space = client::TextSpace::World,
        .position = {1.0F, 2.0F, 3.0F},
        .scale = 0.75F,
    };
    client::TextColor const color{0.25F, 0.5F, 0.75F, 1.0F};

    ASSERT_TRUE(renderer.submit("Block label", color, placement));
    ASSERT_EQ(renderer.labels().size(), 1U);
    EXPECT_EQ(renderer.labels()[0].value, "Block label");
    EXPECT_EQ(renderer.labels()[0].placement.space, client::TextSpace::World);
    EXPECT_EQ(renderer.labels()[0].placement.position[2], 3.0F);
    EXPECT_EQ(renderer.labels()[0].color.packed(), color.packed());
}

TEST(TextRendererTest, UsesOneFallbackGlyphForEachUnsupportedUtf8Character)
{
    client::TextDocument document;
    ASSERT_TRUE(client::appendText(document, "caf\xC3\xA9 \xE4\xB8\x96"));
    client::TextBatch batch;
    client::packText(document, batch);

    ASSERT_EQ(batch.glyphs.size(), 6U);
    EXPECT_EQ(batch.glyphs[3].character, static_cast<uint32_t>('?'));
    EXPECT_EQ(batch.glyphs[5].character, static_cast<uint32_t>('?'));
}

TEST(TextRendererTest, ScalesScreenGlyphSizeAlongWithSpacing)
{
    client::TextDocument document;
    ASSERT_TRUE(client::appendText(document, "AB"));
    client::TextBatch batch;
    client::packText(document, batch, { .scale = 1.5F });

    ASSERT_EQ(batch.glyphs.size(), 2U);
    EXPECT_FLOAT_EQ(batch.glyphs[0].width, 24.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[0].height, 48.0F);
    EXPECT_FLOAT_EQ(batch.glyphs[1].x, 24.0F);
}

TEST(TextRendererTest, MapsScreenAndWorldBatchesToTheirShaderModes)
{
    EXPECT_EQ(
        client::textShaderMode(client::TextSpace::Screen),
        client::TextShaderMode::Screen
    );
    EXPECT_EQ(
        client::textShaderMode(client::TextSpace::World),
        client::TextShaderMode::World
    );
    EXPECT_EQ(static_cast<uint32_t>(client::TextShaderMode::Screen), 0U);
    EXPECT_EQ(static_cast<uint32_t>(client::TextShaderMode::World), 1U);
}
