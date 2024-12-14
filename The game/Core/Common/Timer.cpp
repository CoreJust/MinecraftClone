#include "Timer.hpp"

core::common::Timer g_timer;

core::common::Timer::Timer() 
    : m_startTime(Time::now()) {
}

core::common::Timer::Timer(Time const startTime) noexcept
	: m_startTime(startTime) {
}

core::common::Timer core::common::Timer::global() noexcept {
	return g_timer;
}

core::common::Time core::common::Timer::startTime() const noexcept {
	return m_startTime;
}

core::common::Duration core::common::Timer::elapsed() const noexcept {
	return Time::now() - m_startTime;
}

void core::common::Timer::restart() {
    m_startTime = Time::now();
}

bool core::common::Timer::tryRestartWithInterval(Duration interval) {
	if (elapsed() >= interval) {
		restart();
		return true;
	}
	return false;
}
