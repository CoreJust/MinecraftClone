// Refer to docs/code/graphics/How to declare new Vulkan resources.md for additional info.

#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/TrivialPair.hpp>
#include <core/vulkan/internal/VulkanFwd.hpp>

#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

/* 
 * Must be added in the beginning of each Vulkan resource.
 * SelfType is the type of the resource that's being declared.
 * Variable arguments must include list of fields required to
 * destroy the object and optional `CORE_VK_BATCH_DESTROYABLE()`
 * which enables batch destruction.
 */ 
#define CORE_VK_RESOURCE_CONTEXT(SelfType, ...)              \
public:                                                      \
    using Self = SelfType;                                   \
    using Parent = ::core::VulkanResourceBase<Self::Handle>; \
    static constexpr auto NAME = #SelfType;                  \
                                                             \
    struct Destroyer final {                                 \
        __VA_ARGS__                                          \
        void operator()(Self& self);                         \
    };                                                       \
                                                             \
    constexpr SelfType() noexcept = default;

#define CORE_VK_BATCH_DESTROYABLE() \
    void operator()(std::vector<Self>& selves);

#define CORE_VK_RESOURCE_CONSTRUCTION_FROM(...)     \
public:                                             \
    template<typename... Args> [[nodiscard]]        \
    static Self make(Args&&... args) {              \
        Self result{ };                             \
        constructInplace(                           \
            result,                                 \
            std::forward<Args>(args)...);           \
        return result;                              \
    }                                               \
                                                    \
    template<typename... Args> [[nodiscard]]        \
    static auto makeWithDestroyer(Args&&... args) { \
        TrivialPair<Self, Destroyer> result{ };     \
        constructInplace(                           \
            result.first,                           \
            std::forward<Args>(args)...,            \
            &result.second);                        \
        return result;                              \
    }                                               \
private:                                            \
    static void constructInplace(                   \
        Self& self,                                 \
        __VA_ARGS__,                                \
        Destroyer* destroyer = nullptr              \
    )

#define CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM(...) \
public:                                               \
    using BatchConstructible = void;                  \
    template<typename... Args> [[nodiscard]]          \
    static std::span<Self> batchMake(                 \
        std::span<Self> result, Args&&... args) {     \
        batchConstructInplace(                        \
            result,                                   \
            std::forward<Args>(args)...);             \
        return result;                                \
    }                                                 \
                                                      \
    template<typename... Args> [[nodiscard]]          \
    static auto batchMakeWithDestroyer(               \
        std::span<Self> result, Args&&... args) {     \
        Destroyer d{ };                               \
        batchConstructInplace(                        \
            result,                                   \
            std::forward<Args>(args)...,              \
            &d);                                      \
        return TrivialPair{ result, std::move(d) };   \
    }                                                 \
                                                      \
    template<typename... Args> [[nodiscard]]          \
    static Self make(Args&&... args) {                \
        Self result{ };                               \
        batchConstructInplace(                        \
            std::span(&result, 1),                    \
            std::forward<Args>(args)...);             \
        return result;                                \
    }                                                 \
                                                      \
    template<typename... Args> [[nodiscard]]          \
    static auto makeWithDestroyer(Args&&... args) {   \
        TrivialPair<Self, Destroyer> result{ };       \
        constructInplace(                             \
            std::span(&result.first, 1),              \
            std::forward<Args>(args)...,              \
            &result.second);                          \
        return result;                                \
    }                                                 \
private:                                              \
    static void batchConstructInplace(                \
        std::span<Self>& selves,                      \
        __VA_ARGS__,                                  \
        Destroyer* destroyer = nullptr                \
    )

#define CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(Self, ...) \
    void Self::constructInplace(Self& self, __VA_ARGS__, Destroyer* destroyer)

#define CORE_VK_RESOURCE_DEFERRED_BATCH_CONSTRUCTION_IMPL(Self, ...) \
    void Self::batchConstructInplace(std::span<Self>& selves, __VA_ARGS__, Destroyer* destroyer)

#define CORE_VK_CAPTURE_DESTRUCTION_CONTEXT() \
    if (destroyer)                            \
        *destroyer = Destroyer

// Must be declarared in a source file with resource destruction.
#define CORE_VK_RESOURCE_DESTROY_IMPL(Self) \
    void Self::Destroyer::operator()(Self& self)
#define CORE_VK_RESOURCE_BATCH_DESTROY_IMPL(Self) \
    void Self::Destroyer::operator()(std::vector<Self>& selves)

namespace core {

template<typename HandleTy>
class VulkanResourceBase {
public:
    using Handle = HandleTy;
public:
    constexpr VulkanResourceBase() noexcept = default;
    constexpr explicit VulkanResourceBase(Handle const handle) noexcept
        : m_handle(handle)
    { }

    [[nodiscard]]
    constexpr Handle handle() const noexcept { return m_handle; }
    [[nodiscard]]
    constexpr Handle* handlePtr() & noexcept { return &m_handle; }
    [[nodiscard]]
    constexpr bool isNull() const noexcept { return m_handle == VK_NULL_HANDLE; }
protected:
    Handle m_handle = VK_NULL_HANDLE;
};

template<typename T, typename Resource>
concept VulkanResourceDestoyer = requires (T& destroyer, Resource& res) {
    destroyer(res);
    requires std::is_default_constructible_v<std::remove_cvref_t<T>>;
};

template<typename T, typename Destroyer>
concept VulkanResourceConceptImpl = VulkanResourceDestoyer<Destroyer, T> && requires {
    requires std::is_default_constructible_v<T>;
    requires std::is_trivially_copyable_v<T>;
    requires std::is_trivially_move_constructible_v<T>;

    requires std::is_trivially_move_constructible_v<Destroyer>;
    typename T::Handle;
    typename T::Parent;
    T::NAME;

    requires std::is_base_of_v<VulkanResourceBase<typename T::Handle>, T>;
};

template<typename T>
concept VulkanResource = VulkanResourceConceptImpl<std::remove_cvref_t<T>, typename std::remove_cvref_t<T>::Destroyer>;

template<typename T>
concept VulkanBatchResource = VulkanResource<T> && requires { typename std::remove_cvref_t<T>::BatchConstructible; };


///   RAII WRAPPERS   ///

template<VulkanResource Resource>
class VulkanRaii : public Resource, NonCopyable {
    using Destroyer = Resource::Destroyer;
public:
    VulkanRaii() noexcept = default;
    VulkanRaii(VulkanRaii&& other) noexcept 
        : Resource(other.grabRaw())
        , m_destroyer(std::move(other.m_destroyer))
    { }

    VulkanRaii& operator=(VulkanRaii&& other) noexcept {
        if (this != &other) {
            this->~VulkanRaii();
            static_cast<Resource&>(*this) = other.grabRaw();
            m_destroyer = std::move(other.m_destroyer);
        }
        return *this;
    }

    template<typename... Args>
    explicit VulkanRaii(Args&&... args)
        : VulkanRaii{ Resource::makeWithDestroyer(std::forward<Args>(args)...) }
    { }

    ~VulkanRaii() noexcept {
        Resource& self = static_cast<Resource&>(*this);
        if (!self.isNull()) {
            m_destroyer(self);
            self = Resource{ };
        }
    }

    [[nodiscard]]
    Destroyer      & destroyer()      & noexcept { return m_destroyer; }
    [[nodiscard]]
    Destroyer const& destroyer() const& noexcept { return m_destroyer; }

    [[nodiscard]]
    Resource raw() const noexcept { return static_cast<Resource const&>(*this); }
    [[nodiscard]]
    Resource grabRaw() noexcept { return std::exchange(static_cast<Resource&>(*this), Resource{ }); }
private:
    explicit VulkanRaii(TrivialPair<Resource, Destroyer> rd)
        : Resource{ std::move(rd.first) }
        , m_destroyer{ std::move(rd.second) }
    { }
private:
    Destroyer m_destroyer;
};

template<VulkanBatchResource Resource>
class VulkanRaiiVector : public NonCopyable {
    using Destroyer = Resource::Destroyer;
    using Resources = std::vector<Resource>;
public:
    VulkanRaiiVector() noexcept = default;
    VulkanRaiiVector(VulkanRaiiVector&& other) noexcept = default;
    VulkanRaiiVector& operator=(VulkanRaiiVector&& other) noexcept = default;

    template<typename... Args>
    explicit VulkanRaiiVector(size_t const size, Args&&... args)
        : m_resources(size)
        , m_destroyer(Resource::batchMakeWithDestroyer(m_resources, std::forward<Args>(args)...).second)
    { }

    ~VulkanRaiiVector() noexcept {
        if constexpr(requires{ std::declval<Destroyer>()(m_resources); }) {
            if (!m_resources.empty()) {
                m_destroyer(m_resources);
            }
        } else {
            for (Resource& resource : m_resources) {
                if (!resource.isNull()) {
                    m_destroyer(resource);
                }
            }
        }
    }

    [[nodiscard]]
    Resource& operator[](size_t const i) noexcept { return m_resources[i]; }
    [[nodiscard]]
    Resource const& operator[](size_t const i) const noexcept { return m_resources[i]; }

    [[nodiscard]]
    constexpr size_t size() const noexcept { return m_resources.size(); }
    [[nodiscard]]
    constexpr bool empty() const noexcept { return m_resources.empty(); }

    [[nodiscard]]
    Resources& resources() noexcept { return m_resources; }
    [[nodiscard]]
    Resources const& resources() const noexcept { return m_resources; }
    [[nodiscard]]
    Destroyer      & destroyer()      & noexcept { return m_destroyer; }
    [[nodiscard]]
    Destroyer const& destroyer() const& noexcept { return m_destroyer; }
private:
    Resources m_resources;
    Destroyer m_destroyer;
};

} // namespace core
