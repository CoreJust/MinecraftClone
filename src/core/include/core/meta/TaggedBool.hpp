#pragma once

#include <string_view>

namespace core {

template<typename Tag>
struct TaggedBool final {
    using TagType = Tag;

    bool value;

    static const TaggedBool<Tag> Yes;
    static const TaggedBool<Tag> No;

    [[nodiscard]]
    explicit constexpr operator bool() const noexcept { return value; }

    [[nodiscard]]
    constexpr std::string_view toStringView() const noexcept {
        return value ? "Yes" : "No";
    }
};

template<typename Tag>
constexpr inline TaggedBool<Tag> TaggedBool<Tag>::Yes{ true };
template<typename Tag>
constexpr inline TaggedBool<Tag> TaggedBool<Tag>::No{ false };

template<typename To, typename Tag>
    requires requires { typename To::TagType; }
[[nodiscard]]
constexpr To alias_cast(TaggedBool<Tag> const from) noexcept {
    return To{ from.value };
}

} // namespace core
