#pragma once

#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/SpirV.hpp>
#include <core/vulkan/enum/ShaderStage.hpp>

CORE_VK_ERROR(PipelineLayoutCreationFailed, VulkanRuntimeError);

namespace core::vk {

struct PushConstantRange final {
    ShaderStages stage_flags;
    uint32_t offset = 0;
    uint32_t size;
};

class RawPipelineLayout : public VulkanResourceBase<VkPipelineLayout> {
public:
    struct Info final {
        std::vector<PushConstantRange> push_constant_ranges;
        // TODO: add descriptor sets

        [[nodiscard]]
        static Info fromSpirVs(std::span<SpirV const*> const spir_vs);

        [[nodiscard]]
        static Info fromSpirVs(auto&&... spir_vs)
            requires (std::is_same_v<SpirV, std::remove_cvref_t<decltype(spir_vs)>> && ...)
        {
            SpirV const* spir_vs_arr[] = { &spir_vs... };
            return fromSpirVs(std::span{ spir_vs_arr });
        }
    };
private:
    CORE_VK_RESOURCE_CONTEXT(RawPipelineLayout,
        VkDevice device_handle;);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(Device const& device, Info const& info);
};

using PipelineLayout = VulkanRaii<RawPipelineLayout>;

} // namespace core::vk
