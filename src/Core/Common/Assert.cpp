#include "Assert.hpp"
#include <stacktrace>
#include <Core/IO/Logger.hpp>
#include <Core/OS/Debug.hpp>

namespace core::common {
	void assert_failure_impl(char const* const condition, char const* const file, unsigned long long line, char const* const message) {
		io::error(
			"Assertion failed: {} evaluated to false; {}\nat {}:{}\nStacktrace:\n{}",
			condition,
			message,
			file,
			line,
			std::stacktrace::current());
		os::debugBreak();
	}
} // namespace core::common
