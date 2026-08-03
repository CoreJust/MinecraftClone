#include <core/vulkan/Extensions.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace core::vk;

namespace {

bool isInstanceExtension(VulkanExtension const ext) {
    return getExtensionKind(ext) == VulkanExtensionKind::Instance;
}

} // namespace

TEST(VulkanExtensionsTest, DefaultConstructedHasNoExtensionsPresent) {
    VulkanExtensions const extensions{ };
    for (VulkanExtension const ext : core::valuesOf<VulkanExtension>()) {
        EXPECT_FALSE(extensions.hasExtension(ext)) << "ext index " << core::indexOf(ext);
    }
}

TEST(VulkanExtensionsTest, DefaultConstructedVersionsUseAbsenceSentinel) {
    VulkanExtensions const extensions{ };
    for (VulkanExtension const ext : core::valuesOf<VulkanExtension>()) {
        EXPECT_EQ(extensions.getExtensionVersion(ext).epoch, 0xFFFF'FFFFu) << "ext index " << core::indexOf(ext);
    }
}

TEST(VulkanExtensionsTest, SettingVersionMakesExtensionPresent) {
    VulkanExtensions extensions{ };
    core::Version const version{ 0, 1, 2, 3 };
    extensions.versionAt(VulkanExtension::Swapchain) = version;
    EXPECT_TRUE(extensions.hasExtension(VulkanExtension::Swapchain));
    EXPECT_EQ(extensions.getExtensionVersion(VulkanExtension::Swapchain).epoch, version.epoch);
    EXPECT_EQ(extensions.getExtensionVersion(VulkanExtension::Swapchain).major, version.major);
    EXPECT_EQ(extensions.getExtensionVersion(VulkanExtension::Swapchain).minor, version.minor);
    EXPECT_EQ(extensions.getExtensionVersion(VulkanExtension::Swapchain).patch, version.patch);
}

TEST(VulkanExtensionsTest, SettingOneExtensionDoesNotAffectOthers) {
    VulkanExtensions extensions{ };
    extensions.versionAt(VulkanExtension::Maintenance1) = core::Version{ 0, 1, 1, 0 };
    EXPECT_TRUE(extensions.hasExtension(VulkanExtension::Maintenance1));
    EXPECT_FALSE(extensions.hasExtension(VulkanExtension::Maintenance2));
    EXPECT_FALSE(extensions.hasExtension(VulkanExtension::Swapchain));
}

TEST(VulkanExtensionsTest, FullNameRoundTripsThroughFromFullNameForEveryExtension) {
    for (VulkanExtension const ext : core::valuesOf<VulkanExtension>()) {
        std::string const name = getFullExtensionName(ext);
        ASSERT_FALSE(name.empty()) << "empty name for ext index " << core::indexOf(ext);
        EXPECT_FALSE(name.back() == ' ') << "trailing space in name '" << name << "'";
        auto const parsed = extensionFromFullName(name);
        ASSERT_TRUE(parsed.has_value()) << "extensionFromFullName failed for name '" << name << "'";
        EXPECT_EQ(*parsed, ext) << "round-trip mismatch for name '" << name << "'";
    }
}

TEST(VulkanExtensionsTest, ExtensionFromFullNameReturnsNulloptForUnknownNames) {
    EXPECT_FALSE(extensionFromFullName("").has_value());
    EXPECT_FALSE(extensionFromFullName("VK_KHR_not_a_real_extension").has_value());
    EXPECT_FALSE(extensionFromFullName("VK_KHR_android_surface ").has_value()); // trailing space
    EXPECT_FALSE(extensionFromFullName("vk_khr_android_surface").has_value()); // wrong case
    EXPECT_FALSE(extensionFromFullName("VK_KHR_adnroid_surface").has_value()); // the old typo
}

TEST(VulkanExtensionsTest, ExtensionKindClassifiesEverySurfaceAsInstance) {
    for (VulkanExtension const ext : core::valuesOf<VulkanExtension>()) {
        bool const is_surface_like = false
            || ext == VulkanExtension::Surface
            || ext == VulkanExtension::AndroidSurface
            || ext == VulkanExtension::DirectfbSurface
            || ext == VulkanExtension::FuchsiaImagepipeSurface
            || ext == VulkanExtension::HeadlessSurface
            || ext == VulkanExtension::MetalSurface
            || ext == VulkanExtension::OhosSurface
            || ext == VulkanExtension::QnxSurface
            || ext == VulkanExtension::UbmSurface
            || ext == VulkanExtension::Win32Surface
            || ext == VulkanExtension::WaylandSurface
            || ext == VulkanExtension::XcbSurface
            || ext == VulkanExtension::XlibSurface
            || ext == VulkanExtension::PortabilityEnumeration
            || ext == VulkanExtension::DebugUtils
        ;
        EXPECT_EQ(isInstanceExtension(ext), is_surface_like) << "ext index " << core::indexOf(ext);
    }
    EXPECT_EQ(getExtensionKind(VulkanExtension::Swapchain), VulkanExtensionKind::Device);
    EXPECT_EQ(getExtensionKind(VulkanExtension::CreateRenderPass2), VulkanExtensionKind::Device);
    EXPECT_EQ(getExtensionKind(VulkanExtension::MeshShader), VulkanExtensionKind::Device);
    EXPECT_EQ(getExtensionKind(VulkanExtension::Maintenance1), VulkanExtensionKind::Device);
}

namespace {

void expectPromotedTo(VulkanExtension const ext, uint32_t const major, uint32_t const minor) {
    core::Version const v = getExtensionPromotionVersion(ext);
    EXPECT_EQ(v.epoch, 0u) << "ext index " << core::indexOf(ext);
    EXPECT_EQ(v.major, major) << "ext index " << core::indexOf(ext);
    EXPECT_EQ(v.minor, minor) << "ext index " << core::indexOf(ext);
    EXPECT_EQ(v.patch, 0u) << "ext index " << core::indexOf(ext);
}

} // namespace

TEST(VulkanExtensionsTest, PromotionVersionMatchesVulkanCoreForPromotedExtensions) {
    expectPromotedTo(VulkanExtension::Maintenance1, 1, 1);
    expectPromotedTo(VulkanExtension::Maintenance2, 1, 1);
    expectPromotedTo(VulkanExtension::Maintenance3, 1, 1);
    expectPromotedTo(VulkanExtension::BindMemory2, 1, 1);
    expectPromotedTo(VulkanExtension::DedicatedAllocation, 1, 1);
    expectPromotedTo(VulkanExtension::ExternalMemory, 1, 1);
    expectPromotedTo(VulkanExtension::ExternalSemaphore, 1, 1);
    expectPromotedTo(VulkanExtension::GetMemoryRequirements2, 1, 1);
    expectPromotedTo(VulkanExtension::CreateRenderPass2, 1, 2);
    expectPromotedTo(VulkanExtension::BufferDeviceAddress, 1, 2);
    expectPromotedTo(VulkanExtension::DescriptorIndexing, 1, 2);
    expectPromotedTo(VulkanExtension::Maintenance4, 1, 3);
    expectPromotedTo(VulkanExtension::Maintenance5, 1, 4);
    expectPromotedTo(VulkanExtension::Maintenance6, 1, 4);
    expectPromotedTo(VulkanExtension::HostImageCopy, 1, 4);
    expectPromotedTo(VulkanExtension::ToolingInfo, 1, 4);
}

TEST(VulkanExtensionsTest, NonPromotedExtensionsReportMaxVersion) {
    EXPECT_TRUE(getExtensionPromotionVersion(VulkanExtension::Swapchain) >= core::Version::MAX());
    EXPECT_TRUE(getExtensionPromotionVersion(VulkanExtension::MeshShader) >= core::Version::MAX());
    EXPECT_TRUE(getExtensionPromotionVersion(VulkanExtension::Surface) >= core::Version::MAX());
    EXPECT_TRUE(getExtensionPromotionVersion(VulkanExtension::DebugUtils) >= core::Version::MAX());
}

TEST(VulkanExtensionsTest, ToStringListsPresentExtensionsOnly) {
    VulkanExtensions extensions{ };
    extensions.versionAt(VulkanExtension::Swapchain) = core::Version{ 0, 1, 0, 0 };
    extensions.versionAt(VulkanExtension::Maintenance1) = core::Version{ 0, 1, 1, 0 };
    std::string const text = extensions.toString();
    EXPECT_NE(text.find("VK_KHR_swapchain"), std::string::npos);
    EXPECT_NE(text.find("VK_KHR_maintenance1"), std::string::npos);
    EXPECT_EQ(text.find("VK_KHR_swapchain"), text.rfind("VK_KHR_swapchain"));
    EXPECT_EQ(text.find("VK_EXT_mesh_shader"), std::string::npos);
}
