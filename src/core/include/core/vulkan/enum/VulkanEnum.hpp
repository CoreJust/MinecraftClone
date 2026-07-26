#pragma once

#include <core/meta/Enum.hpp>
#include <core/meta/EnumMapping.hpp>

namespace core::vk {

template<typename VkEnum, CountableEnum E>
    requires std::is_enum_v<VkEnum>
constexpr VkEnum toVk(E const e) noexcept {
    return static_cast<VkEnum>(EnumMapping<E, uint32_t>::map(e));
}

template<CountableEnum E, typename VkEnum>
    requires std::is_enum_v<VkEnum>
constexpr E fromVk(VkEnum const e) noexcept {
    return EnumMapping<E, uint32_t>::unmap(static_cast<uint32_t>(e));
}

} // namespace core::vk

/*
 * Requires a corresponding CORE_ENUM_FUNCTIONS_IMPL in a source file.
 * Allows to provide custom anchor points for conversion to and from raw Vulkan types, e.g.:
 * CORE_VK_REGISTER_ENUM(AttachmentLoadOp, { None, 1'000'400'000 });
 */
#define CORE_VK_REGISTER_ENUM(E, ...)   \
    CORE_ENUM_FUNCTIONS(::core::vk:: E) \
    __VA_OPT__(CORE_DEFINE_ENUM_MAPPING(::core::vk:: E, uint32_t, __VA_ARGS__))
