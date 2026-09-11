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
