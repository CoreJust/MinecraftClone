#pragma once
#include <Core/Macro/Attributes.hpp>
#include "Int.hpp"

namespace core {
	// A light-weight struct to work with the time
	struct Duration final {
		usize ns;

		PURE constexpr static Duration Us(usize us) noexcept {
			return { us * 1'000 };
		}

		PURE constexpr static Duration Ms(usize ms) noexcept {
			return { ms * 1'000'000 };
		}

		PURE constexpr static Duration Seconds(usize seconds) noexcept {
			return { seconds * 1'000'000'000 };
		}

		PURE constexpr static Duration Minutes(usize minutes) noexcept {
			return { minutes * 60'000'000'000 };
		}

		PURE constexpr static Duration Hours(usize hours) noexcept {
			return { hours * 3'600'000'000'000 };
		}

		PURE constexpr static Duration Days(usize days) noexcept {
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

		PURE constexpr usize asUs() const noexcept {
			return ns / 1'000;
		}

		PURE constexpr usize asMs() const noexcept {
			return ns / 1'000'000;
		}

		PURE constexpr double asSeconds() const noexcept {
			return static_cast<double>(ns) / 1'000'000'000.0;
		}

		PURE constexpr usize asSecondsWhole() const noexcept {
			return ns / 1'000'000'000;
		}

		PURE constexpr usize us() const noexcept {
			return (ns / 1'000) % 1'000;
		}

		PURE constexpr usize ms() const noexcept {
			return (ns / 1'000'000) % 1'000;
		}

		PURE constexpr usize seconds() const noexcept {
			return (ns / 1'000'000'000) % 60;
		}

		PURE constexpr usize minutes() const noexcept {
			return (ns / 60'000'000'000) % 60;
		}

		PURE constexpr usize hours() const noexcept {
			return (ns / 3'600'000'000'000) % 24;
		}

		PURE constexpr usize days() const noexcept {
			return (ns / 86'400'000'000'000);
		}


	};

	// A light-weight struct to work with the time
	struct Time final {
		usize ns;

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
} // namespace core
