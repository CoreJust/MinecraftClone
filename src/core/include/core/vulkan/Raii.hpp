#pragma once

#include <core/common/NonCopyable.hpp>

#include <type_traits>
#include <utility>

namespace core {

template<typename T, typename Resource>
concept VulkanResourceDestoyer = requires (T& destroyer, Resource& res) {
    destroyer(res);
};

template<typename T>
concept VulkanResource = requires (T res) {
    requires std::is_default_constructible_v<std::remove_cvref_t<T>>;
    requires std::is_trivially_copyable_v<std::remove_cvref_t<T>>;
    requires std::is_trivially_move_constructible_v<std::remove_cvref_t<T>>;

    typename std::remove_cvref_t<T>::Destroyer;
    requires VulkanResourceDestoyer<typename std::remove_cvref_t<T>::Destroyer, std::remove_cvref_t<T>>;

    res.handle();
};

template<VulkanResource Resource>
class VulkanRaii : public Resource, NonCopyable {
    using Destroyer = Resource::Destroyer;
public:
    VulkanRaii() noexcept = default;

    template<typename... Args>
    explicit VulkanRaii(Args&&... args)
        : Resource{ std::forward<Args>(args)..., m_destroyer }
    { }

    ~VulkanRaii() noexcept {
        m_destroyer(static_cast<Resource&>(*this));
    }

    Destroyer      & destroyer()      & noexcept { return m_destroyer; }
    Destroyer const& destroyer() const& noexcept { return m_destroyer; }

    Resource raw() const noexcept { return static_cast<Resource&>(*this); }
    Resource grabRaw() noexcept { return std::exchange(static_cast<Resource&>(*this), Resource{ }); }
private:
    Destroyer m_destroyer;
};

} // namespace core
