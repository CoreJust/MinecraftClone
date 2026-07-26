#include <core/vulkan/PipelineLayout.hpp>

#include <core/vulkan/Check.hpp>

#include <spirv-reflect/spirv_reflect.h>
#include <volk.h>

#include <unordered_map>

namespace core::vk {
namespace {

[[nodiscard]]
ShaderStage reflectStageToShaderStage(SpvReflectShaderStageFlagBits const stage_flags) {
    switch (stage_flags) {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:                  return ShaderStage::Vertex;
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return ShaderStage::TessellationControl;
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return ShaderStage::TesselationEvaluation;
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:                return ShaderStage::Geometry;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:                return ShaderStage::Fragment;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:                 return ShaderStage::Compute;
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT:                return ShaderStage::Task;
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT:                return ShaderStage::Mesh;
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:              return ShaderStage::Raygen;
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:             return ShaderStage::AnyHit;
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:         return ShaderStage::ClosestHit;
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:                return ShaderStage::Miss;
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR:        return ShaderStage::Intersection;
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR:            return ShaderStage::Callable;
    default: UNREACHABLE("Unknown SpvReflectShaderStageFlagBits: {}", static_cast<uint32_t>(stage_flags));
    }
}

} // namespace

RawPipelineLayout::Info RawPipelineLayout::Info::fromSpirVs(std::span<SpirV const*> const spir_vs) {
    // key = (offset << 32) | size
    std::unordered_map<uint64_t, ShaderStages> combined;

    for (SpirV const* const spir_v : spir_vs) {
        ASSERT(spir_v != nullptr);
        spv_reflect::ShaderModule reflect_module{
            spir_v->data().size_bytes(),
            spir_v->data().data(),
            SpvReflectModuleFlagBits::SPV_REFLECT_MODULE_FLAG_NO_COPY,
        };

        uint32_t const entry_count = reflect_module.GetEntryPointCount();
        for (uint32_t i = 0; i < entry_count; ++i) {
            char const* const ep_name = reflect_module.GetEntryPointName(i);
            ASSERT(ep_name != nullptr, "Null entry point name at index {}", i);

            SpvReflectShaderStageFlagBits const stage_flags = reflect_module.GetEntryPointShaderStage(i);
            ShaderStage const stage = reflectStageToShaderStage(stage_flags);

            SpvReflectBlockVariable const* const push_block = reflect_module.GetEntryPointPushConstantBlock(ep_name);
            if (push_block == nullptr || push_block->size == 0) {
                continue;
            }

            uint64_t const key = (static_cast<uint64_t>(push_block->offset) << 32) | push_block->size;
            if (auto it = combined.find(key); it != combined.end()) {
                it->second |= stage;
            } else {
                for (auto const& [existing_key, _] : combined) {
                    uint32_t const existing_offset = static_cast<uint32_t>(existing_key >> 32);
                    uint32_t const existing_size   = static_cast<uint32_t>(existing_key);
                    ASSERT(
                        !(existing_offset == push_block->offset && existing_size != push_block->size),
                        "Conflicting push constant range at offset {} (existing size {}, new size {})",
                        push_block->offset, existing_size, push_block->size
                    );
                }
                combined.emplace(key, stage);
            }
        }
    }

    std::vector<PushConstantRange> ranges;
    ranges.reserve(combined.size());
    for (auto const& [key, stages] : combined) {
        ranges.push_back(PushConstantRange{
            .stage_flags = stages,
            .offset = static_cast<uint32_t>(key >> 32),
            .size = static_cast<uint32_t>(key),
        });
    }

    std::sort(
        ranges.begin(), ranges.end(),
        [](PushConstantRange const& a, PushConstantRange const& b) { return a.offset < b.offset; }
    );

    return Info{ .push_constant_ranges = std::move(ranges) };
}

CORE_VK_RESOURCE_DESTROY_IMPL(RawPipelineLayout) {
    vkDestroyPipelineLayout(device_handle, self.m_handle, nullptr);
}

CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawPipelineLayout,
    Device const& device,
    Info const& info
) {
    VkPipelineLayoutCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = static_cast<uint32_t>(info.push_constant_ranges.size()),
        .pPushConstantRanges = reinterpret_cast<VkPushConstantRange const*>(info.push_constant_ranges.data()),
    };
    if (!VK_CHECK(vkCreatePipelineLayout(device.handle(), &create_info, nullptr, &self.m_handle))) {
        throw PipelineLayoutCreationFailed{ "Failed to create pipeline layout" };
    }
    CORE_DEBUG(
        "Created Vulkan pipeline layout with push constants ranges: {}",
        core::joinFmt(info.push_constant_ranges, [](fmt::context::iterator out, PushConstantRange const& range){
            return fmt::format_to(out, "{} offset {} size {}", core::joinFmt(range.stage_flags), range.offset, range.size);
        })
    );
    CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ .device_handle = device.handle() };
}

} // namespace core::vk
