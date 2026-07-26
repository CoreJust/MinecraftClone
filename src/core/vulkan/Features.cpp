#include <core/vulkan/Features.hpp>

#include <core/meta/EnumImpl.hpp>

#include <fmt/core.h>

CORE_ENUM_FUNCTIONS_IMPL(::core::vk::VulkanFeature);

namespace core::vk {

std::string VulkanFeatures::toString(std::string_view const indent) const {
    std::string features_message;
    for (VulkanFeature const feature : valuesOf<VulkanFeature>()) {
        if (this->operator[](feature)) {
            features_message += fmt::format("{}{:40}\n", indent, feature);
        }
    }
    return features_message;
}

} // namespace core::vk
