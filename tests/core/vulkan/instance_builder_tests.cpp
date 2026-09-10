#include <core/vulkan/builder/InstanceBuilder.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace core::vk;

TEST(InstanceBuilderTest, EmptyExtensionRequirementsPreservePreviousRequirements)
{
    InstanceBuilder builder;
    builder.requireExtensions({ VulkanExtension::Surface });
    builder.requireExtensions({});
    builder.requireExtensions({ VulkanExtension::DebugUtils });

    EXPECT_TRUE(std::ranges::contains(builder.requiredExtensions(), VulkanExtension::Surface));
    EXPECT_TRUE(std::ranges::contains(builder.requiredExtensions(), VulkanExtension::DebugUtils));
}

TEST(InstanceBuilderTest, SuccessiveExtensionPreferencesAccumulate)
{
    InstanceBuilder builder;
    builder.preferExtensions({ VulkanExtension::Surface });
    builder.preferExtensions({ VulkanExtension::DebugUtils });

    EXPECT_TRUE(std::ranges::contains(builder.preferredExtensions(), VulkanExtension::Surface));
    EXPECT_TRUE(std::ranges::contains(builder.preferredExtensions(), VulkanExtension::DebugUtils));
}
