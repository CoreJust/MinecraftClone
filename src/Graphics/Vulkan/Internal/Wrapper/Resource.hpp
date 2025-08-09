#pragma once
#include <Core/Container/DynArray.hpp>
#include "Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class ResourceBase {
    protected:
        VkAnyHandle m_value = VK_NULL_HANDLE;

    public:
        constexpr ResourceBase() noexcept = default;
        constexpr ResourceBase(ResourceBase&& other) noexcept : m_value(other.m_value) { other.m_value = VK_NULL_HANDLE; }
        constexpr ResourceBase(ResourceBase const&) noexcept = default;
        constexpr ResourceBase(VkAnyHandle value) noexcept : m_value(value) { }
        constexpr ResourceBase& operator=(ResourceBase&& other) &noexcept {
            if (this != &other) {
                VkAnyHandle tmp = m_value;
                m_value = other.m_value;
                other.m_value = tmp;
            }
            return *this;
        }
        constexpr ResourceBase& operator=(ResourceBase const&) &noexcept = default;
        constexpr ~ResourceBase() { m_value = VK_NULL_HANDLE; }

        constexpr explicit operator bool() const noexcept { return m_value != VK_NULL_HANDLE; }
    };

    template<typename T>
    class Resource : public ResourceBase {
        static_assert(sizeof(T) == sizeof(VkAnyHandle));

    public:
        using ResourceType = T;

    public:
        constexpr Resource() noexcept = default;
        constexpr Resource(Resource&&) noexcept = default;
        constexpr Resource(Resource const&) noexcept = default;
        constexpr Resource(T value) noexcept : ResourceBase(reinterpret_cast<VkAnyHandle>(value)) { }
        constexpr Resource& operator=(Resource&&) &noexcept = default;
        constexpr Resource& operator=(Resource const&) &noexcept = default;

        static void release(Vulkan& vulkan, core::ArrayView<T> items, void* allocator = nullptr) noexcept;

        PURE T  get() const noexcept { return reinterpret_cast<T> (m_value); }
        PURE T* ptr()       noexcept { return reinterpret_cast<T*>(&m_value); }
    
    protected:
        void set(T value) noexcept { m_value = reinterpret_cast<VkAnyHandle>(value); }
    };

    template<typename T>
    class StandaloneResource : public Resource<T> {
#define DERIVED_STANDALONE_RESOURCE(T)        \
        using Parent = StandaloneResource<T>; \
        using Parent::get;                    \
        using Parent::set;                    \
        using Parent::m_vulkan

    protected:
        Vulkan& m_vulkan;
        void*   m_allocator = nullptr;

    public:
        StandaloneResource(Vulkan& vulkan, void* allocator = nullptr)
            : m_vulkan(vulkan)
            , m_allocator(allocator)
        { }
        StandaloneResource(StandaloneResource&&) noexcept = default;
        StandaloneResource& operator=(StandaloneResource&&) noexcept = default;
        ~StandaloneResource() noexcept { Resource<T>::release(m_vulkan, { this->ptr(), 1 }, m_allocator); }
    };

    template<typename T>
    class ResourceSet {
#define DERIVED_RESOURCE_SET(T)        \
        using Parent = ResourceSet<T>; \
        using Parent::m_resources;     \
        using Parent::m_vulkan

    public:
        using ResourceType = Resource<T>;

    protected:
        core::DynArray<ResourceType> m_resources;
        Vulkan& m_vulkan;
        void* m_allocator = nullptr;

    public:
        ResourceSet(Vulkan& vulkan, usize count, void* allocator = nullptr)
            : m_resources(count)
            , m_vulkan   (vulkan)
            , m_allocator(allocator)
        { }
        ~ResourceSet() { ResourceType::release(m_vulkan, m_resources.view().template reinterpret<T>(), m_allocator); }

        ResourceType      & operator[]  (usize i)       noexcept { return m_resources[i]; }
        ResourceType const& operator[]  (usize i) const noexcept { return m_resources[i]; }
        core::ArrayView<ResourceType> resources   () const noexcept { return m_resources; }
        u32                           size32      () const noexcept { return static_cast<u32>(m_resources.size()); }
        T*                            resourcesPtr()       noexcept { return reinterpret_cast<T*>(m_resources.data()); }
    };
} // namespace graphics::vulkan::internal
