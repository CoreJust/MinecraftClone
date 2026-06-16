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

template<typename T>
    requires std::is_enum_v<T> && requires(T t) { to_string(t); }
struct VulkanErrorOverEnum : public VulkanError {
    T kind;

    VulkanErrorOverEnum(T const kind)
        : VulkanError(to_string(kind))
        , kind(kind)
    { }

    template<typename... Args>
    VulkanErrorOverEnum(T const kind, fmt::format_string<Args...> format_string, Args&&... args)
        : VulkanError(to_string(kind) + ": " + fmt::format(format_string, std::forward<Args>(args)...))
        , kind(kind)
    { }
};

} // namespace core
