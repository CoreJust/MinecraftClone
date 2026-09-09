#pragma once

#include <core/common/Version.hpp>
#include <core/vulkan/Capabilities.hpp>

#include <algorithm>

namespace client::detail {

inline constexpr core::Version MESH_SHADER_SPIRV_VULKAN_VERSION{ 0, 1, 3, 0 };

[[nodiscard]]
inline bool shouldUseMeshShaderPipelines(
    bool const prefer_mesh_shaders,
    core::vk::VulkanCaps const& caps
) noexcept {
    return prefer_mesh_shaders
        && caps.hasMeshShaders()
        && std::min(caps.instanceVersion(), caps.deviceVersion()) >= MESH_SHADER_SPIRV_VULKAN_VERSION;
}

} // namespace client::detail
