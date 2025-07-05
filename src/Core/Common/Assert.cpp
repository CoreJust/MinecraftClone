#include "Assert.hpp"
#include <Core/IO/Logger.hpp>

namespace core::common {
	void assert_failure(std::string_view message, const std::source_location& location) {
		io::error(
			"Assertion failed in {} at {}:{}, function {} : {}",
			location.file_name(),
			location.line(),
			location.column(),
			location.function_name(),
			message);
	}
} // namespace core::common
