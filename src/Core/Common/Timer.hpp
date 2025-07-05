#pragma once
#include "Time.hpp"

namespace core::common {
	class Timer final {
		Time m_startTime;

	public:
		Timer();
		Timer(Time const startTime) noexcept;

		static Timer global() noexcept;

		Time startTime() const noexcept;
		[[nodiscard]] Duration elapsed() const noexcept;
		void restart();

		// If the elapsed time is no less than the interval, restarts the timer and returns true
		// Otherwise, returns false
		bool tryRestartWithInterval(Duration interval);
	};
} // namespace core::common
