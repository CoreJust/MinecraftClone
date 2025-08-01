#include "ShaderModule.hpp"
#include <vulkan/vulkan.h>
#include <Resources.hpp>
#include <Core/IO/Logger.hpp>
#include <Core/IO/File.hpp>
#include <Graphics/Vulkan/Exception.hpp>
#include "../Check.hpp"
#include "../Vulkan/Vulkan.hpp"

namespace graphics::vulkan::internal {
    ShaderModule::ShaderModule(Vulkan& vulkan, char const* const path) 
        : m_vulkan(vulkan) {
        core::info("Loading shader module from {}", path);
        std::string const spirVSource = core::readFile(makeShaderPath(path));

        VkShaderModuleCreateInfo createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = spirVSource.size();
        createInfo.pCode = reinterpret_cast<const u32*>(spirVSource.data());

        m_shaderModule = m_vulkan.create<vkCreateShaderModule>(&createInfo, nullptr);
    }

    ShaderModule::~ShaderModule() {
        m_vulkan.destroy<vkDestroyShaderModule>(m_shaderModule, nullptr);
    }

    VkPipelineShaderStageCreateInfo ShaderModule::makeCreationInfo(VkShaderStageFlagBits stage) const {
        VkPipelineShaderStageCreateInfo createInfo { };
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        createInfo.stage = stage;
        createInfo.module = m_shaderModule;
        createInfo.pName = "main";
        return createInfo;
    }
} // namespace graphics::vulkan::internal
