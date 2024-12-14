#pragma once
#include <string_view>
#include <source_location>

namespace core::common {
	void assert_failure(std::string_view message = "", const std::source_location& location = std::source_location::current());
}

#define ASSERT(expr, msg) if (!(expr)) core::common::assert_failure(msg)
