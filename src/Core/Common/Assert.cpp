#include "Assert.hpp"
#include <stacktrace>
#include <Core/IO/Logger.hpp>

namespace core::common {
	void assert_failure(std::string_view message) {
		io::error(
			"Assertion: {}\nat {}",
			message,
			std::stacktrace::current());
	}
} // namespace core::common
