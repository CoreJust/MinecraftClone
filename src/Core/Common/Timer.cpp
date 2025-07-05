#include "Timer.hpp"

namespace core::common {
	Timer g_timer;

	Timer::Timer() 
		: m_startTime(Time::now()) {
	}

	Timer::Timer(Time const startTime) noexcept
		: m_startTime(startTime) {
	}

	Timer Timer::global() noexcept {
		return g_timer;
	}

	Time Timer::startTime() const noexcept {
		return m_startTime;
	}

	Duration Timer::elapsed() const noexcept {
		return Time::now() - m_startTime;
	}

	void Timer::restart() {
		m_startTime = Time::now();
	}

	bool Timer::tryRestartWithInterval(Duration interval) {
		if (elapsed() >= interval) {
			restart();
			return true;
		}
		return false;
	}
} // namespace core::common
