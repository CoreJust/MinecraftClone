#pragma once
#include <core/common/Assert.hpp>

#include <vulkan/vulkan.h>

namespace core::internal {
[[nodiscard]]
bool checkVkResult(VkResult const result);
} // namespace core::internal

#define VK_CHECK(...) ::core::internal::checkVkResult(__VA_ARGS__)
#define VK_ASSERT(...) ASSERT(VK_CHECK(__VA_ARGS__))
