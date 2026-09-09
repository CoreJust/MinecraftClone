#include "../../src/client/render/MeshShaderSelection.hpp"

#include <gtest/gtest.h>

using namespace core::vk;

namespace {

VulkanCaps makeMeshShaderCaps(
    core::Version const instance_version,
    core::Version const device_version
) {
    VulkanExtensions extensions;
    extensions.versionAt(VulkanExtension::MeshShader) = { 0, 1, 0, 0 };
    VulkanFeatures features;
    features[VulkanFeature::MeshShader] = true;

    VulkanCaps caps;
    caps.commitInstanceCaps(instance_version, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps(
        "Gpu",
        device_version,
        {},
        {},
        PhysicalDeviceType::Other,
        extensions,
        features
    );
    caps.commitDeviceCaps(extensions, features);
    return caps;
}

} // namespace

TEST(MeshShaderSelectionTest, RequiresVulkan13AtBothInstanceAndDevice)
{
    static constexpr core::Version VULKAN_12{ 0, 1, 2, 0 };
    static constexpr core::Version VULKAN_13{ 0, 1, 3, 0 };

    EXPECT_FALSE(client::detail::shouldUseMeshShaderPipelines(true, makeMeshShaderCaps(VULKAN_12, VULKAN_13)));
    EXPECT_FALSE(client::detail::shouldUseMeshShaderPipelines(true, makeMeshShaderCaps(VULKAN_13, VULKAN_12)));
    EXPECT_TRUE(client::detail::shouldUseMeshShaderPipelines(true, makeMeshShaderCaps(VULKAN_13, VULKAN_13)));
}

TEST(MeshShaderSelectionTest, AllowsExplicitVertexFallback)
{
    static constexpr core::Version VULKAN_13{ 0, 1, 3, 0 };

    EXPECT_FALSE(client::detail::shouldUseMeshShaderPipelines(false, makeMeshShaderCaps(VULKAN_13, VULKAN_13)));
}
