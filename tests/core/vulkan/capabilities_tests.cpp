#include <core/vulkan/Capabilities.hpp>

#include <gtest/gtest.h>

using namespace core::vk;

namespace {

void expectVersion(core::Version const& v, uint32_t epoch, uint32_t major, uint32_t minor, uint32_t patch = 0) {
    EXPECT_EQ(v.epoch, epoch);
    EXPECT_EQ(v.major, major);
    EXPECT_EQ(v.minor, minor);
    EXPECT_EQ(v.patch, patch);
}

VulkanCaps makeCapsWithVersions(core::Version const instance_version, core::Version const device_version) {
    VulkanCaps caps;
    caps.commitInstanceCaps(instance_version, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps({}, device_version, {}, {}, PhysicalDeviceType::Other, {}, {});
    caps.commitDeviceCaps({}, {});
    return caps;
}

VulkanExtensions singleExtension(VulkanExtension const ext, core::Version const version = { 0, 1, 0, 0 }) {
    VulkanExtensions extensions;
    extensions.versionAt(ext) = version;
    return extensions;
}

VulkanFeatures singleFeature(VulkanFeature const feature) {
    VulkanFeatures features;
    features[feature] = true;
    return features;
}

} // namespace

TEST(VulkanCapabilitiesTest, FreshlyDefaultConstructedReportsZerosAndDefaults) {
    VulkanCaps const caps;
    expectVersion(caps.instanceVersion(), 0, 1, 0, 0);
    expectVersion(caps.deviceVersion(), 0, 1, 0, 0);
    EXPECT_TRUE(caps.deviceName().empty());
    EXPECT_FALSE(caps.validationEnabled());
    EXPECT_EQ(caps.swapchainImageCount(), 0u);
    EXPECT_EQ(caps.extent().x, 0u);
    EXPECT_EQ(caps.extent().y, 0u);
}

TEST(VulkanCapabilitiesTest, CommitInstanceCapsStoresVersionsAndFlags) {
    VulkanCaps caps;
    VulkanLayers supported_layers;
    VulkanLayers enabled_layers;
    supported_layers.versionAt(VulkanLayer::Validation) = core::Version{ 0, 1, 3, 0 };
    enabled_layers.versionAt(VulkanLayer::Validation) = core::Version{ 0, 1, 3, 0 };
    VulkanExtensions supported_exts = singleExtension(VulkanExtension::Surface, { 0, 25, 0, 0 });
    VulkanExtensions enabled_exts = singleExtension(VulkanExtension::Surface);

    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, true, supported_layers, enabled_layers, supported_exts, enabled_exts);

    expectVersion(caps.instanceVersion(), 0, 1, 3, 0);
    EXPECT_TRUE(caps.validationEnabled());
    EXPECT_TRUE(caps.has(VulkanLayer::Validation));
    EXPECT_TRUE(caps.has(VulkanExtension::Surface));
    expectVersion(caps.deviceVersion(), 0, 1, 0, 0);
}

TEST(VulkanCapabilitiesTest, CommitPhysicalDeviceCapsStoresNameAndVersion) {
    VulkanCaps caps;
    VulkanExtensions supported_exts = singleExtension(VulkanExtension::Swapchain, { 0, 1, 0, 0 });

    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps("TestGPU", core::Version{ 0, 1, 2, 0 }, {}, {}, PhysicalDeviceType::Discrete, supported_exts, {});

    EXPECT_EQ(caps.deviceName(), "TestGPU");
    expectVersion(caps.deviceVersion(), 0, 1, 2, 0);
    ASSERT_FALSE(caps.supportedDeviceExtensionsAsVec().empty());
    EXPECT_EQ(caps.supportedDeviceExtensionsAsVec().front(), VulkanExtension::Swapchain);
    EXPECT_FALSE(caps.has(VulkanExtension::Swapchain));
}

TEST(VulkanCapabilitiesTest, CommitDeviceCapsPopulatesEnabledExtensionAndFeatureSets) {
    VulkanCaps caps;
    VulkanExtensions supported = singleExtension(VulkanExtension::Swapchain);
    VulkanFeatures supported_features = singleFeature(VulkanFeature::MeshShader);
    VulkanExtensions enabled = singleExtension(VulkanExtension::Swapchain);
    VulkanFeatures enabled_features = singleFeature(VulkanFeature::DynamicRendering);

    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps(
        "Gpu",
        core::Version{ 0, 1, 3, 0 },
        {},
        {},
        PhysicalDeviceType::Other,
        supported, supported_features
    );
    caps.commitDeviceCaps(enabled, enabled_features);

    EXPECT_TRUE(caps.has(VulkanExtension::Swapchain));
    EXPECT_TRUE(caps.has(VulkanFeature::DynamicRendering));
    EXPECT_FALSE(caps.has(VulkanFeature::MeshShader));
}

TEST(VulkanCapabilitiesTest, CommitSwapchainCapsStoresAllFields) {
    VulkanCaps caps;
    caps.commitSwapchainCaps(
        Format::R8G8B8A8UNorm,
        ColorSpace::SRGBNonlinear,
        PresentMode::FIFO,
        SurfaceTransformBits{ 1 },
        Extent2d{ 1920, 1080 },
        3
    );

    EXPECT_EQ(caps.swapchainImageCount(), 3u);
    EXPECT_EQ(caps.extent().x, 1920u);
    EXPECT_EQ(caps.extent().y, 1080u);
}

TEST(VulkanCapabilitiesTest, HasExtensionReflectsEnabledSetOnly) {
    VulkanCaps caps = makeCapsWithVersions(core::Version{ 0, 1, 3, 0 }, core::Version{ 0, 1, 3, 0 });
    EXPECT_FALSE(caps.has(VulkanExtension::Swapchain));
    caps.commitDeviceCaps(singleExtension(VulkanExtension::Swapchain), {});
    EXPECT_TRUE(caps.has(VulkanExtension::Swapchain));
    EXPECT_FALSE(caps.has(VulkanExtension::Maintenance1));
}

TEST(VulkanCapabilitiesTest, HasFeatureReflectsEnabledSetOnly) {
    VulkanCaps caps;
    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps(
        "Gpu",
        core::Version{ 0, 1, 3, 0 },
        {},
        {},
        PhysicalDeviceType::Other,
        {},
        singleFeature(VulkanFeature::DynamicRendering)
    );
    caps.commitDeviceCaps({}, singleFeature(VulkanFeature::Synchronization2));
    EXPECT_TRUE(caps.has(VulkanFeature::Synchronization2));
    EXPECT_FALSE(caps.has(VulkanFeature::DynamicRendering)); // supported but not enabled
}

TEST(VulkanCapabilitiesTest, HasLayerReflectsEnabledSetOnly) {
    VulkanCaps caps;
    VulkanLayers enabled_layers;
    enabled_layers.versionAt(VulkanLayer::Validation) = core::Version{ 0, 1, 3, 0 };
    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, enabled_layers, {}, {});
    EXPECT_TRUE(caps.has(VulkanLayer::Validation));
    EXPECT_FALSE(caps.has(VulkanLayer::ApiDump));
}

///   hasMeshShaders()   ///

TEST(VulkanCapabilitiesTest, HasMeshShadersRequiresBothExtensionAndFeature) {
    VulkanCaps caps;
    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps("Gpu", core::Version{ 0, 1, 3, 0 }, {}, {}, PhysicalDeviceType::Other, {}, {});

    EXPECT_FALSE(caps.hasMeshShaders());
    caps.commitDeviceCaps(singleExtension(VulkanExtension::MeshShader), {});
    EXPECT_FALSE(caps.hasMeshShaders()); // extension only

    VulkanFeatures features = singleFeature(VulkanFeature::MeshShader);
    caps.commitDeviceCaps(singleExtension(VulkanExtension::MeshShader), features);
    EXPECT_TRUE(caps.hasMeshShaders());
}

TEST(VulkanCapabilitiesTest, HasMeshShadersIsFalseWhenOnlyFeatureEnabled) {
    VulkanCaps caps;
    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps("Gpu", core::Version{ 0, 1, 3, 0 }, {}, {}, PhysicalDeviceType::Other, {}, {});
    caps.commitDeviceCaps({}, singleFeature(VulkanFeature::MeshShader));
    EXPECT_FALSE(caps.hasMeshShaders());
}

TEST(VulkanCapabilitiesTest, PromotedDeviceExtensionAvailableViaCoreVersionEvenWhenNotEnabled) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 3, 0 }, core::Version{ 0, 1, 2, 0 });
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::CreateRenderPass2));
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance1));
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::BufferDeviceAddress));
}

TEST(VulkanCapabilitiesTest, PromotedDeviceExtensionUnavailableBelowPromotionVersionWhenNotEnabled) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 3, 0 }, core::Version{ 0, 1, 1, 0 });
    EXPECT_FALSE(caps.hasExtensionOrPromoted(VulkanExtension::CreateRenderPass2)); // needs 1.2
    EXPECT_FALSE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance4)); // needs 1.3
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance1)); // 1.1, satisfied
}

TEST(VulkanCapabilitiesTest, PromotedDeviceExtensionAvailableAtExactlyPromotionVersion) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 3, 0 }, core::Version{ 0, 1, 3, 0 });
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance4));
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::DescriptorIndexing)); // 1.2
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance3)); // 1.1
}

TEST(VulkanCapabilitiesTest, DeviceVersion1_4SatisfiesAll14Promotions) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 4, 0 }, core::Version{ 0, 1, 4, 0 });
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance5));
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Maintenance6));
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::HostImageCopy));
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::ToolingInfo));
}

TEST(VulkanCapabilitiesTest, NonPromotedDeviceExtensionReturnsFalseWhenNeitherVersionNorEnabled) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 4, 0 }, core::Version{ 0, 1, 4, 0 });
    EXPECT_FALSE(caps.hasExtensionOrPromoted(VulkanExtension::Swapchain));
    EXPECT_FALSE(caps.hasExtensionOrPromoted(VulkanExtension::MeshShader));
}

TEST(VulkanCapabilitiesTest, DeviceExtensionFallsBackToEnabledSetWhenNotPromoted) {
    VulkanCaps caps;
    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps({}, core::Version{ 0, 1, 3, 0 }, {}, {}, PhysicalDeviceType::Other, {}, {});
    caps.commitDeviceCaps(singleExtension(VulkanExtension::Swapchain), {});
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Swapchain));
}

TEST(VulkanCapabilitiesTest, EnabledDeviceExtensionStillReportsTrueEvenWhenVersionAlsoSatisfies) {
    VulkanCaps caps;
    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps({}, core::Version{ 0, 1, 2, 0 }, {}, {}, PhysicalDeviceType::Other, {}, {});
    caps.commitDeviceCaps(singleExtension(VulkanExtension::CreateRenderPass2), {});
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::CreateRenderPass2));
}

TEST(VulkanCapabilitiesTest, InstanceExtensionFallsBackToEnabledSet) {
    VulkanCaps caps;
    caps.commitInstanceCaps(core::Version{ 0, 1, 1, 0 }, false, {}, {}, {}, singleExtension(VulkanExtension::Surface));
    caps.commitPhysicalDeviceCaps({}, core::Version{ 0, 1, 1, 0 }, {}, {}, PhysicalDeviceType::Other, {}, {});
    caps.commitDeviceCaps({}, {});
    EXPECT_TRUE(caps.hasExtensionOrPromoted(VulkanExtension::Surface));
}

TEST(VulkanCapabilitiesTest, InstanceExtensionReturnsFalseWhenNotEnabledAndBelowPromotion) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 1, 0 }, core::Version{ 0, 1, 1, 0 });
    EXPECT_FALSE(caps.hasExtensionOrPromoted(VulkanExtension::Surface));
}

TEST(VulkanCapabilitiesTest, SupportedDeviceExtensionsAsVecListsOnlySupportedDeviceExtensions) {
    VulkanCaps caps;
    VulkanExtensions supported;
    supported.versionAt(VulkanExtension::Swapchain) = core::Version{ 0, 1, 0, 0 };
    supported.versionAt(VulkanExtension::MeshShader) = core::Version{ 0, 1, 0, 0 };
    supported.versionAt(VulkanExtension::Surface) = core::Version{ 0, 1, 0, 0 };

    caps.commitInstanceCaps(core::Version{ 0, 1, 3, 0 }, false, {}, {}, {}, {});
    caps.commitPhysicalDeviceCaps("Gpu", core::Version{ 0, 1, 3, 0 }, {}, {}, PhysicalDeviceType::Other, supported, {});

    std::vector<VulkanExtension> const vec = caps.supportedDeviceExtensionsAsVec();
    ASSERT_EQ(vec.size(), 2u);
    EXPECT_NE(std::find(vec.begin(), vec.end(), VulkanExtension::Swapchain), vec.end());
    EXPECT_NE(std::find(vec.begin(), vec.end(), VulkanExtension::MeshShader), vec.end());
    EXPECT_EQ(std::find(vec.begin(), vec.end(), VulkanExtension::Surface), vec.end());
}

TEST(VulkanCapabilitiesTest, SupportedDeviceExtensionsAsVecIsEmptyWhenNoneSupported) {
    auto const caps = makeCapsWithVersions(core::Version{ 0, 1, 3, 0 }, core::Version{ 0, 1, 3, 0 });
    EXPECT_TRUE(caps.supportedDeviceExtensionsAsVec().empty());
}
