#pragma once

#include <core/meta/Enum.hpp>

#include <fmt/core.h>

#include <stdexcept>

namespace core::vk {

struct VulkanError : public std::runtime_error {
    explicit VulkanError(std::string message)
        : std::runtime_error(std::move(message))
    { }

    template<typename... Args>
    explicit VulkanError(fmt::format_string<Args...> format_string, Args&&... args)
        : std::runtime_error(fmt::format(format_string, std::forward<Args>(args)...))
    { }
};

struct VulkanInitializationError : public VulkanError {
    using VulkanError::VulkanError;
};

struct VulkanFrameGraphBuildError : public VulkanError {
    using VulkanError::VulkanError;
};

struct VulkanRuntimeError : public VulkanError {
    using VulkanError::VulkanError;
};

// Must be placed out of any namespace.
#define CORE_VK_ERROR(Name, Base, ...) \
namespace core::vk {                   \
    struct Name final : public Base {  \
        using Base::Base;              \
        __VA_ARGS__                    \
    };                                 \
} // namespace core::vk

// There must he a corresponding CORE_ENUM_FUNCTIONS_IMPL(NameKind) in a source file
// Must be placed out pf any namespace
#define CORE_VK_ERROR_WITH_KINDS(Name, Base, ...)  \
namespace core::vk {                               \
    enum class Name##Kind { __VA_ARGS__, Count, }; \
}                                                  \
    CORE_ENUM_FUNCTIONS(::core::vk::Name##Kind);   \
    CORE_VK_ERROR(Name, Base,                      \
        using enum Name##Kind;                     \
        Name##Kind kind;                           \
                                                   \
        Name(Name##Kind const kind)                \
            : Base(std::string{ toStringView(kind) }) \
            , kind(kind)                           \
        { }                                        \
        template<typename... Args>                 \
        Name(                                      \
            Name##Kind const kind,                 \
            fmt::format_string<Args...> format_string, \
            Args&&... args)                        \
            : Base("{}: {}",                       \
                toStringView(kind),                \
                fmt::format(                       \
                    format_string,                 \
                    std::forward<Args>(args)...))  \
            , kind(kind)                           \
        { }                                        \
    )

} // namespace core::vk
