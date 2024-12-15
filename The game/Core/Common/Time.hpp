#pragma once
#include <Core/Macro/Attributes.hpp>

namespace core::common {
	// A light-weight struct to work with the time
	struct Duration final {
		size_t ns;

		PURE constexpr static Duration Us(size_t us) noexcept {
			return { us * 1'000 };
		}

		PURE constexpr static Duration Ms(size_t ms) noexcept {
			return { ms * 1'000'000 };
		}

		PURE constexpr static Duration Seconds(size_t seconds) noexcept {
			return { seconds * 1'000'000'000 };
		}

		PURE constexpr static Duration Minutes(size_t minutes) noexcept {
			return { minutes * 60'000'000'000 };
		}

		PURE constexpr static Duration Hours(size_t hours) noexcept {
			return { hours * 3'600'000'000'000 };
		}

		PURE constexpr static Duration Days(size_t days) noexcept {
			return { days * 86'400'000'000'000 };
		}

		PURE constexpr bool operator==(Duration other) const noexcept {
			return ns == other.ns;
		}

		PURE constexpr bool operator!=(Duration other) const noexcept {
			return ns != other.ns;
		}

		PURE constexpr bool operator<=(Duration other) const noexcept {
			return ns <= other.ns;
		}

		PURE constexpr bool operator>=(Duration other) const noexcept {
			return ns >= other.ns;
		}

		PURE constexpr bool operator<(Duration other) const noexcept {
			return ns < other.ns;
		}

		PURE constexpr bool operator>(Duration other) const noexcept {
			return ns > other.ns;
		}

		PURE constexpr Duration operator+(Duration other) const noexcept {
			return { ns + other.ns };
		}

		constexpr Duration& operator+=(Duration other) noexcept {
			ns += other.ns;
			return *this;
		}

		PURE constexpr Duration operator-(Duration other) const noexcept {
			return { ns - other.ns };
		}

		constexpr Duration& operator-=(Duration other) noexcept {
			ns -= other.ns;
			return *this;
		}

		PURE constexpr size_t asUs() const noexcept {
			return ns / 1'000;
		}

		PURE constexpr size_t asMs() const noexcept {
			return ns / 1'000'000;
		}

		PURE constexpr double asSeconds() const noexcept {
			return ns / 1'000'000'000.0;
		}

		PURE constexpr size_t asSecondsWhole() const noexcept {
			return ns / 1'000'000'000;
		}

		PURE constexpr size_t us() const noexcept {
			return (ns / 1'000) % 1'000;
		}

		PURE constexpr size_t ms() const noexcept {
			return (ns / 1'000'000) % 1'000;
		}

		PURE constexpr size_t seconds() const noexcept {
			return (ns / 1'000'000'000) % 60;
		}

		PURE constexpr size_t minutes() const noexcept {
			return (ns / 60'000'000'000) % 60;
		}

		PURE constexpr size_t hours() const noexcept {
			return (ns / 3'600'000'000'000) % 24;
		}

		PURE constexpr size_t days() const noexcept {
			return (ns / 86'400'000'000'000);
		}


	};

	// A light-weight struct to work with the time
	struct Time final {
		size_t ns;

		PURE static Time now();

		PURE constexpr bool operator==(Time other) const noexcept {
			return ns == other.ns;
		}

		PURE constexpr Duration operator-(Time other) const noexcept {
			return { ns - other.ns };
		}

		PURE constexpr Time operator+(Duration duration) const noexcept {
			return { ns + duration.ns };
		}

		constexpr Time& operator+=(Duration duration) noexcept {
			ns += duration.ns;
			return *this;
		}
	};
}
