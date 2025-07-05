#pragma once
#include <string_view>
#include <source_location>

namespace core::common {
	void assert_failure(std::string_view message = "", const std::source_location& location = std::source_location::current());
} // namespace core::common

#define ASSERT(expr, ...) if (!(expr)) ::core::common::assert_failure(__VA_ARGS__)
