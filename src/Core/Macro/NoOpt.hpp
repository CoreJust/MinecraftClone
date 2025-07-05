#pragma once
#include <Core/Macro/Attributes.hpp>

#if defined(_MSC_VER)
#include <intrin.h>
namespace core::macro {
	void useVolatile(char const volatile* const ptr) noexcept;

	INLINE void NoOptImpl(auto const& value) noexcept {
		useVolatile(&reinterpret_cast<char const volatile&>(value));
		_ReadWriteBarrier();
	}
} // namespace core::macro
#else
namespace core::macro {
	INLINE void NoOptImpl(auto const& value) noexcept {
		asm volatile("" : : "r,m"(value) : "memory");
	}

	INLINE void NoOptImpl(auto& value) noexcept {
#if defined(__clang__)
		asm volatile("" : "+r,m"(value) : : "memory");
#else
		asm volatile("" : "+m,r"(value) : : "memory");
#endif
}
} // namespace core::macro
#endif

#define NO_OPT(...) ::core::macro::NoOptImpl(__VA_ARGS__)
