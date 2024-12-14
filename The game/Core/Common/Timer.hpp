#pragma once
#include "Time.hpp"

namespace core::common {
	class Timer final {
		Time m_startTime;

	public:
		Timer();

		static Timer global() noexcept;

		[[nodiscard]] Duration elapsed() const noexcept;
		void restart();

		// If the elapsed time is no less than the interval, restarts the timer and returns true
		// Otherwise, returns false
		bool tryRestartWithInterval(Duration interval);
	};
}