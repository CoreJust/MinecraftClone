#include <core/vulkan/builder/InstanceBuilder.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <span>

using namespace core::vk;

TEST(InstanceBuilderTest, EmptyExtensionRequirementsPreservePreviousRequirements)
{
    InstanceBuilder builder;
    VulkanExtension surface[] = { VulkanExtension::Surface };
    VulkanExtension debug_utils[] = { VulkanExtension::DebugUtils };
    builder.requireExtensions(surface);
    builder.requireExtensions(std::span<VulkanExtension const>{});
    builder.requireExtensions(debug_utils);

    EXPECT_TRUE(std::ranges::contains(builder.requiredExtensions(), VulkanExtension::Surface));
    EXPECT_TRUE(std::ranges::contains(builder.requiredExtensions(), VulkanExtension::DebugUtils));
}

TEST(InstanceBuilderTest, SuccessiveExtensionPreferencesAccumulate)
{
    InstanceBuilder builder;
    VulkanExtension surface[] = { VulkanExtension::Surface };
    VulkanExtension debug_utils[] = { VulkanExtension::DebugUtils };
    builder.preferExtensions(surface);
    builder.preferExtensions(debug_utils);

    EXPECT_TRUE(std::ranges::contains(builder.preferredExtensions(), VulkanExtension::Surface));
    EXPECT_TRUE(std::ranges::contains(builder.preferredExtensions(), VulkanExtension::DebugUtils));
}
