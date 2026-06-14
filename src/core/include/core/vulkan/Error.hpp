#pragma once

#include <fmt/core.h>

#include <stdexcept>

namespace core {

struct VulkanError : public std::runtime_error{
    explicit VulkanError(std::string message)
        : std::runtime_error(std::move(message))
    { }

    template<typename... Args>
    explicit VulkanError(fmt::format_string<Args...> format_string, Args&&... args)
        : std::runtime_error(fmt::format(format_string, std::forward<Args>(args)...))
    { }
};

} // namespace core
