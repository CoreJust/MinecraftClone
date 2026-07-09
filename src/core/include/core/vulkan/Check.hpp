#pragma once

#include <core/common/Assert.hpp>

#include <volk.h>

namespace core::vk::internal {
[[nodiscard]]
bool checkVkResult(VkResult const result);
} // namespace core::vk::internal

#define VK_CHECK(...) ::core::vk::internal::checkVkResult(__VA_ARGS__)
#define CORE_VK_ASSERT(...) ASSERT(VK_CHECK(__VA_ARGS__))
