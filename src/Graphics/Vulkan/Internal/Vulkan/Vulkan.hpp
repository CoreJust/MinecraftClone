#pragma once
#include <typeinfo>
#include <Core/Meta/FunctionDecomposition.hpp>
#include <Core/Meta/IsSame.hpp>
#include <Core/Meta/UnPtr.hpp>
#include <Core/Container/UniquePtr.hpp>
#include <Core/Memory/Forward.hpp>
#include <Core/Common/NonMovable.hpp>
#include <Core/Common/Version.hpp>
#include <Core/Container/DynArray.hpp>
#include "../Memory/Allocator.hpp"
#include "../Check.hpp"
#include "Instance.hpp"
#include "Surface.hpp"
#include "PhysicalDevice.hpp"
#include "Device.hpp"
#include "Swapchain.hpp"

namespace graphics::window {
    class Window;
}

namespace graphics::vulkan::internal {
    class Vulkan final : core::NonMovable {
        core::UniquePtr<Instance>       m_instance;
        core::UniquePtr<Surface>        m_surface;
        core::UniquePtr<PhysicalDevice> m_physicalDevice;
        core::UniquePtr<Device>         m_device;
        core::UniquePtr<Allocator>      m_allocator;
        core::UniquePtr<Swapchain>      m_swapchain;

        template<auto Func>
        PURE INLINE auto firstArgumentOf() const noexcept {
            using Result = core::FirstArgumentOf<decltype(Func)>;
            if constexpr (core::IsSame<Result, VkDevice>) {
                return m_device->get();
            } else if constexpr (core::IsSame<Result, VkPhysicalDevice>) {
                return m_physicalDevice->get();
            } else if constexpr (core::IsSame<Result, VkInstance>) {
                return m_instance->get();
            } else if constexpr (core::IsSame<Result, VmaAllocator>) {
                return m_allocator->get();
            } else if constexpr (core::IsSame<Result, VkSurfaceKHR>) {
                return m_surface->get();
            } else if constexpr (core::IsSame<Result, VkSwapchainKHR>) {
                return m_swapchain->get();
            } else {
                static_assert(false, "Expected first argument to be either VkDevice, VkPhysicalDevice, VkInstance, VkSurfaceKHR, or VkSwapchainKHR");
            }
        }

    public:
        Vulkan(window::Window& win, core::Version const& appVersion);
        Vulkan(Vulkan&&) noexcept = delete;
        Vulkan(Vulkan const&) noexcept = delete;
        Vulkan& operator=(Vulkan&&) noexcept = delete;
        Vulkan& operator=(Vulkan const&) noexcept = delete;
        ~Vulkan();

        void recreateSwapchain(core::Vec2u32 pixelSize);

        template<typename T>
        PURE INLINE core::UniquePtr<T> make(auto&&... args) {
            return core::makeUP<T>(*this, FORWARD(args)...);
        }
        
        template<auto Func>
        PURE INLINE auto create(auto&&... args) const {
            using Result = core::UnPtr<core::LastArgumentOf<decltype(Func)>>;
            Result result = VK_NULL_HANDLE;
            if (!VK_CHECK(Func(m_device->get(), FORWARD(args)..., &result)))
                creationFailure(typeid(Result).name());
            return result;
        }

        template<auto Func>
        INLINE bool destroy(auto& resource, auto&&... args) const {
            if (resource != VK_NULL_HANDLE) {
                call<Func>(resource, FORWARD(args)...);
                resource = VK_NULL_HANDLE;
                return true;
            }
            return false;
        }

        template<auto Func>
        PURE INLINE auto enumerate(auto&&... args) const {
            using T = core::UnPtr<core::LastArgumentOf<decltype(Func)>>;
            using Result = core::DynArray<T>;
            u32 count = 0;
            if constexpr (core::IsSame<core::ReturnTypeOf<decltype(Func)>, VkResult>) {
                if (!VK_CHECK(Func(firstArgumentOf<Func>(), args..., &count, nullptr)) || count == 0)
                    return Result { };
                Result result(static_cast<usize>(count));
                if (!VK_CHECK(Func(firstArgumentOf<Func>(), args..., &count, result.data())))
                    return Result { };
                return result;
            } else {
                Func(firstArgumentOf<Func>(), args..., &count, nullptr);
                if (count == 0)
                    return Result { };
                Result result(static_cast<usize>(count));
                Func(firstArgumentOf<Func>(), args..., &count, result.data());
                return result;
            }
        }

        template<auto Func>
        INLINE bool safeCall(auto&&... args) const {
            return VK_CHECK(Func(firstArgumentOf<Func>(), FORWARD(args)...));
        }

        template<auto Func>
        INLINE auto call(auto&&... args) const {
            return Func(firstArgumentOf<Func>(), FORWARD(args)...);
        }

        PURE Instance            & instance()             noexcept { return *m_instance; }
        PURE Surface             & surface()              noexcept { return *m_surface; }
        PURE PhysicalDevice      & physicalDevice()       noexcept { return *m_physicalDevice; }
        PURE Device              & device()               noexcept { return *m_device; }
        PURE Allocator           & allocator()            noexcept { return *m_allocator; }
        PURE Swapchain           & swapchain()            noexcept { return *m_swapchain; }
        PURE Instance       const& instance()       const noexcept { return *m_instance; }
        PURE Surface        const& surface()        const noexcept { return *m_surface; }
        PURE PhysicalDevice const& physicalDevice() const noexcept { return *m_physicalDevice; }
        PURE QueueFamilies  const& queueFamilies()  const noexcept { return m_physicalDevice->queueFamilies(); }
        PURE Device         const& device()         const noexcept { return *m_device; }
        PURE Allocator      const& allocator()      const noexcept { return *m_allocator; }
        PURE Swapchain      const& swapchain()      const noexcept { return *m_swapchain; }

    private:
        [[noreturn]]
        static void creationFailure(char const* const resourceName);
    };
} // namespace graphics::vulkan::internal
