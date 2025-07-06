#include "ShaderModule.hpp"
#include <Core/IO/Logger.hpp>
#include <Core/IO/File.hpp>
#include "Check.hpp"
#include "../Exception.hpp"
#include <Resources.hpp>

namespace graphics::vulkan::internal {
    ShaderModule::ShaderModule(VkDevice device, char const* const path) 
        : m_device(device) {
        core::io::info("Loading shader module from {}", path);
        std::string const source = core::io::readFile(makeShaderPath(path));
        core::io::debug("Shader source loaded:\n{}\n", source);

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = source.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(source.data());
        if (!VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &m_shaderModule))) {
            core::io::error("Failed to create shader module {}", path);
            throw VulkanException { };
        }
    }

    ShaderModule::~ShaderModule() {
        if (m_shaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_shaderModule, nullptr);
            m_shaderModule = VK_NULL_HANDLE;
        }
    }
} // namespace graphics::vulkan::internal
