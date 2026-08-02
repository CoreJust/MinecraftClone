#include <core/vulkan/MetalLayer.hpp>

#ifdef OSX

#include <core/vulkan/Check.hpp>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace core::vk {

namespace {

CAMetalLayer* acquireOrCreateLayer(GLFWwindow* const window) {
    NSWindow* const ns_window = glfwGetCocoaWindow(window);
    NSView* const view = ns_window.contentView;
    if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
        return static_cast<CAMetalLayer*>(view.layer);
    }
    CAMetalLayer* const layer = [CAMetalLayer layer];
    layer.contentsScale = ns_window.backingScaleFactor;
    view.layer = layer;
    view.wantsLayer = YES;
    return layer;
}

} // namespace

MetalLayer::MetalLayer(GLFWwindow* const window) noexcept
    : m_layer((__bridge void*)acquireOrCreateLayer(window)) {
    CFRetain(static_cast<CFTypeRef>(m_layer));
}

MetalLayer::~MetalLayer() noexcept {
    if (m_layer != nullptr) {
        CFRelease(static_cast<CFTypeRef>(m_layer));
        m_layer = nullptr;
    }
}

VkSurfaceKHR MetalLayer::createSurface(VkInstance const instance) const noexcept {
    VkMetalSurfaceCreateInfoEXT const create_info{
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .pLayer = (__bridge CAMetalLayer*)m_layer,
    };
    VkSurfaceKHR handle = VK_NULL_HANDLE;
    CORE_VK_ASSERT(vkCreateMetalSurfaceEXT(instance, &create_info, nullptr, &handle));
    return handle;
}

} // namespace core::vk

#endif // OSX
