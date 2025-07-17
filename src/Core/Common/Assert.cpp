#include "Assert.hpp"
#include <stacktrace>
#include <Core/IO/Logger.hpp>
#include <Core/OS/Debug.hpp>

namespace core {
	void assertFailureImpl(char const* const condition, char const* const file, unsigned long long line, char const* const message) {
		error(
			"Assertion failed: {} evaluated to false; {}\nat {}:{}\nStacktrace:\n{}",
			condition,
			message,
			file,
			line,
			std::stacktrace::current());
		debugBreak();
	}
} // namespace core
