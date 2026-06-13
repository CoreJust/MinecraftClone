#pragma once

#include <core/common/Version.hpp>

namespace core {

[[nodiscard]]
Version vkToVersion(uint32_t const vk_version) noexcept;
[[nodiscard]]
uint32_t versionToVk(Version const& version) noexcept;

void setVkVersion(Version const& version) noexcept;
void setDeviceVkVersion(Version const& version) noexcept;

[[nodiscard]]
Version const& getVkVersion() noexcept;
[[nodiscard]]
Version const& getDeviceVkVersion() noexcept;

} // namespace core
