#pragma once
#include <Core/Macro/Attributes.hpp>

#if defined(_MSC_VER)
#include <intrin.h>
void useVolatile(char const volatile* const ptr) noexcept;

INLINE void NoOpt(auto const& value) noexcept {
	useVolatile(&reinterpret_cast<char const volatile&>(value));
	_ReadWriteBarrier();
}
#else
INLINE void NoOpt(auto const& value) noexcept {
	asm volatile("" : : "r,m"(value) : "memory");
}

INLINE void NoOpt(auto& value) noexcept {
#if defined(__clang__)
	asm volatile("" : "+r,m"(value) : : "memory");
#else
	asm volatile("" : "+m,r"(value) : : "memory");
#endif
}
#endif
