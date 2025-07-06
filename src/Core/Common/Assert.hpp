#pragma once

namespace core::common {
	void assert_failure(char const* const condition, char const* const file, unsigned long long line, char const* const message = "");
} // namespace core::common

#ifdef _MC_DEBUG
#define ASSERT(expr, ...) if (!(expr)) ::core::common::assert_failure(#expr, __FILE__, __LINE__, __VA_ARGS__)
#else
#define ASSERT(expr, ...)
#endif
