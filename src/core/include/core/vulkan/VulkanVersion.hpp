#pragma once

#include <core/common/Version.hpp>

namespace core::vk {

[[nodiscard]]
Version vkToVersion(uint32_t const vk_version) noexcept;
[[nodiscard]]
uint32_t versionToVk(Version const& version) noexcept;

} // namespace core::vk
