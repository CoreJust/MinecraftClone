#pragma once

#ifdef _MC_DEBUG
namespace core::common {
	void assert_failure_impl(char const* const condition, char const* const file, unsigned long long line, char const* const message);
	constexpr void assert_failure(char const* const condition, char const* const file, unsigned long long line, char const* const message = "") {
		if consteval {
			throw message;
		} else {
			assert_failure_impl(condition, file, line, message);
		}
	}
} // namespace core::common

#define ASSERT(expr, ...) if (!(expr)) ::core::common::assert_failure(#expr, __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)
#else
#define ASSERT(expr, ...)
#endif
