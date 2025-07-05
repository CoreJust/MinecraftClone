#pragma once
#include <string_view>

namespace core::common {
	void assert_failure(std::string_view message = "");
} // namespace core::common

#define ASSERT(expr, ...) if (!(expr)) ::core::common::assert_failure(__VA_ARGS__)
