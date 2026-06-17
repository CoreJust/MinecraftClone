#pragma once

#include <core/meta/Enum.hpp>

#include <fmt/core.h>

#include <stdexcept>

namespace core {

struct VulkanError : public std::runtime_error {
    explicit VulkanError(std::string message)
        : std::runtime_error(std::move(message))
    { }

    template<typename... Args>
    explicit VulkanError(fmt::format_string<Args...> format_string, Args&&... args)
        : std::runtime_error(fmt::format(format_string, std::forward<Args>(args)...))
    { }
};

// there must he a corresponding CORE_ENUM_FUNCTIONS_IMPL(NameKind) in a source file
#define CORE_VK_ERROR_WITH_KINDS(Name, ...)        \
    enum class Name##Kind { __VA_ARGS__, Count, }; \
    CORE_ENUM_FUNCTIONS(Name##Kind);               \
                                                   \
    struct Name final : public VulkanError {       \
        using enum Name##Kind;                     \
        Name##Kind kind;                           \
                                                   \
        Name(Name##Kind const kind)                \
            : VulkanError(std::string{ toStringView(kind) }) \
            , kind(kind)                           \
        { }                                        \
        template<typename... Args>                 \
        Name(                                      \
            Name##Kind const kind,                 \
            fmt::format_string<Args...> format_string, \
            Args&&... args)                        \
            : VulkanError("{}: {}",                \
                toStringView(kind),                \
                fmt::format(                       \
                    format_string,                 \
                    std::forward<Args>(args)...))  \
            , kind(kind)                           \
        { }                                        \
    };

} // namespace core
