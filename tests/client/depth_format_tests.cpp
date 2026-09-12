#include <client/render/DepthFormat.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>

namespace {

TEST(DepthFormatTest, SelectsTheFirstSupportedCandidateInDeterministicOrder)
{
    std::array const candidates{
        client::DepthFormatSupport{ .format = VK_FORMAT_D32_SFLOAT, .depth_attachment_supported = false },
        client::DepthFormatSupport{ .format = VK_FORMAT_D16_UNORM, .depth_attachment_supported = true },
    };

    EXPECT_EQ(client::selectDepthAttachmentFormat(candidates), VK_FORMAT_D16_UNORM);
}

TEST(DepthFormatTest, RejectsADeviceWithoutAnyDepthAttachmentFormat)
{
    std::array const candidates{
        client::DepthFormatSupport{ .format = VK_FORMAT_D32_SFLOAT, .depth_attachment_supported = false },
        client::DepthFormatSupport{ .format = VK_FORMAT_D16_UNORM, .depth_attachment_supported = false },
    };

    EXPECT_FALSE(client::selectDepthAttachmentFormat(candidates).has_value());
}

TEST(DepthFormatTest, EveryPresentationPipelineDeclaresTheCommonDepthAttachment)
{
    constexpr VkPipelineLayout GRID_LAYOUT = VK_NULL_HANDLE;
    constexpr VkPipelineLayout PLAYER_LAYOUT = VK_NULL_HANDLE;
    constexpr VkPipelineLayout DEBUG_HUD_LAYOUT = VK_NULL_HANDLE;
    constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
    constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
    client::PresentationPipelineDescriptors const descriptors = client::presentationPipelineDescriptors(
        GRID_LAYOUT,
        PLAYER_LAYOUT,
        DEBUG_HUD_LAYOUT,
        COLOR_FORMAT,
        DEPTH_FORMAT
    );

    EXPECT_EQ(descriptors.grid.layout, GRID_LAYOUT);
    EXPECT_EQ(descriptors.player.layout, PLAYER_LAYOUT);
    EXPECT_EQ(descriptors.debug_hud.layout, DEBUG_HUD_LAYOUT);
    EXPECT_EQ(descriptors.grid.color_format, COLOR_FORMAT);
    EXPECT_EQ(descriptors.player.color_format, COLOR_FORMAT);
    EXPECT_EQ(descriptors.debug_hud.color_format, COLOR_FORMAT);
    EXPECT_EQ(descriptors.grid.depth_format, DEPTH_FORMAT);
    EXPECT_EQ(descriptors.player.depth_format, DEPTH_FORMAT);
    EXPECT_EQ(descriptors.debug_hud.depth_format, DEPTH_FORMAT);
    EXPECT_TRUE(descriptors.grid.depth_test_enabled);
    EXPECT_TRUE(descriptors.grid.depth_write_enabled);
    EXPECT_TRUE(descriptors.player.depth_test_enabled);
    EXPECT_TRUE(descriptors.player.depth_write_enabled);
    EXPECT_FALSE(descriptors.debug_hud.depth_test_enabled);
    EXPECT_FALSE(descriptors.debug_hud.depth_write_enabled);
}

TEST(DepthFormatTest, AllocatesOncePerSwapchainImageAndResetsOnRecreate)
{
    client::DepthTargetSelection selection;
    VkImage const first_image = std::bit_cast<VkImage>(uintptr_t{ 1U });
    VkImage const second_image = std::bit_cast<VkImage>(uintptr_t{ 2U });

    EXPECT_TRUE(selection.select(first_image));
    EXPECT_FALSE(selection.select(first_image));
    EXPECT_TRUE(selection.select(second_image));
    EXPECT_EQ(selection.size(), 2U);

    selection.clear();
    EXPECT_EQ(selection.size(), 0U);
    EXPECT_TRUE(selection.select(first_image));
}

} // namespace
