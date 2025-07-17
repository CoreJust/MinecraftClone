#pragma once

#ifdef _MC_DEBUG
namespace core {
	void assertFailureImpl(char const* const condition, char const* const file, unsigned long long line, char const* const message);
	constexpr void assertFailure(char const* const condition, char const* const file, unsigned long long line, char const* const message = "") {
		if consteval {
			throw message;
		} else {
			assertFailureImpl(condition, file, line, message);
		}
	}
} // namespace core

#define ASSERT(expr, ...) if (!(expr)) ::core::assertFailure(#expr, __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)
#else
#define ASSERT(expr, ...)
#endif
