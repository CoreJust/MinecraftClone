#include <client/render/GuiRenderer.hpp>

#include <glm/mat4x4.hpp>

#include <gtest/gtest.h>

#include <string>

TEST(GuiRendererTest, PacksArbitraryLengthTextAsScreenGlyphs)
{
    client::TextRenderer text;
    std::string const message(500U, 'A');
    ASSERT_TRUE(text.submit(message));

    client::GuiRenderer gui;
    gui.begin();
    ASSERT_TRUE(gui.submit(text, glm::mat4{ 1.0F }, 800U, 600U));
    ASSERT_EQ(gui.batches().size(), 1U);
    ASSERT_EQ(gui.text().glyphs.size(), message.size());
    EXPECT_EQ(gui.text().space, client::TextSpace::Screen);
    EXPECT_EQ(gui.text().glyphs.front().character, static_cast<uint32_t>('A'));
    EXPECT_EQ(gui.text().glyphs.back().character, static_cast<uint32_t>('A'));
}

TEST(GuiRendererTest, RejectsWorldLabelsAtScreenSpaceBoundary)
{
    client::TextRenderer text;
    ASSERT_TRUE(text.submit("Stone", {}, {
        .space = client::TextSpace::World,
        .position = { 0.0F, 0.0F, 0.0F },
    }));

    client::GuiRenderer gui;
    gui.begin();
    EXPECT_FALSE(gui.submit(text, glm::mat4{ 1.0F }, 800U, 600U));
    EXPECT_TRUE(gui.empty());
}

TEST(GuiRendererTest, PreservesColoredGlyphsAndPlacement)
{
    client::GuiRenderer gui;
    gui.begin();
    ASSERT_TRUE(gui.submit("GUI", client::TextColor{ 1.0F, 0.0F, 0.0F, 1.0F }));

    ASSERT_FALSE(gui.empty());
    ASSERT_EQ(gui.text().glyphs.size(), 3U);
    EXPECT_EQ(gui.text().glyphs[0].packed_color, 0xFF00'00FFU);
    EXPECT_FLOAT_EQ(gui.text().glyphs[0].x, 0.0F);
    EXPECT_FLOAT_EQ(gui.text().glyphs[0].y, 0.0F);
}

TEST(GuiRendererTest, RejectsWorldBatch)
{
    client::GuiRenderer gui;
    client::TextBatch batch{};
    batch.space = client::TextSpace::World;
    batch.glyphs.push_back({ .character = static_cast<uint32_t>('X') });

    EXPECT_FALSE(gui.submit(batch));
    EXPECT_TRUE(gui.empty());
}
