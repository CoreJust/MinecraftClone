#include <core/vulkan/ShaderModule.hpp>

#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core::vk {

CORE_VK_RESOURCE_DESTROY_IMPL(RawShaderModule) {
    vkDestroyShaderModule(device_handle, self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawShaderModule,
    Device const& device,
    SpirV const& spir_v
) {
    VkShaderModuleCreateInfo const info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = spir_v.data().size_bytes(),
        .pCode = spir_v.data().data(),
    };

    CORE_VK_ASSERT(vkCreateShaderModule(device.handle(), &info, nullptr, &self.m_handle));
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };
}

} // namespace core::vk
