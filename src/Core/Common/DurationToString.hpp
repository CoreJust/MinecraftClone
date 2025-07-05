#pragma once
#include <string_view>
#include "Time.hpp"

namespace core::common {
	// Thread-safe
	std::string_view durationToString(Duration duration) noexcept;
} // namespace core::common
