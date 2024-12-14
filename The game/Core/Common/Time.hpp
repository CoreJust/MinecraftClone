#pragma once

namespace core::common {
	// A light-weight struct to work with the time
	struct Duration final {
		size_t ns;

		[[nodiscard]] constexpr static Duration Us(size_t us) noexcept {
			return { us * 1'000 };
		}

		[[nodiscard]] constexpr static Duration Ms(size_t ms) noexcept {
			return { ms * 1'000'000 };
		}

		[[nodiscard]] constexpr static Duration Seconds(size_t seconds) noexcept {
			return { seconds * 1'000'000'000 };
		}

		[[nodiscard]] constexpr static Duration Minutes(size_t minutes) noexcept {
			return { minutes * 60'000'000'000 };
		}

		[[nodiscard]] constexpr bool operator==(Duration other) const noexcept {
			return ns == other.ns;
		}

		[[nodiscard]] constexpr bool operator!=(Duration other) const noexcept {
			return ns != other.ns;
		}

		[[nodiscard]] constexpr bool operator<=(Duration other) const noexcept {
			return ns <= other.ns;
		}

		[[nodiscard]] constexpr bool operator>=(Duration other) const noexcept {
			return ns >= other.ns;
		}

		[[nodiscard]] constexpr bool operator<(Duration other) const noexcept {
			return ns < other.ns;
		}

		[[nodiscard]] constexpr bool operator>(Duration other) const noexcept {
			return ns > other.ns;
		}

		[[nodiscard]] constexpr Duration operator+(Duration other) const noexcept {
			return { ns + other.ns };
		}

		constexpr Duration& operator+=(Duration other) noexcept {
			ns += other.ns;
			return *this;
		}

		[[nodiscard]] constexpr Duration operator-(Duration other) const noexcept {
			return { ns - other.ns };
		}

		constexpr Duration& operator-=(Duration other) noexcept {
			ns -= other.ns;
			return *this;
		}

		[[nodiscard]] constexpr size_t asUs() const noexcept {
			return ns / 1'000;
		}

		[[nodiscard]] constexpr size_t asMs() const noexcept {
			return ns / 1'000'000;
		}

		[[nodiscard]] constexpr double asSeconds() const noexcept {
			return ns / 1'000'000'000.0;
		}

		[[nodiscard]] constexpr size_t asSecondsWhole() const noexcept {
			return ns / 1'000'000'000;
		}

		[[nodiscard]] constexpr size_t ms() const noexcept {
			return (ns / 1'000'000) % 1'000;
		}

		[[nodiscard]] constexpr size_t seconds() const noexcept {
			return (ns / 1'000'000'000) % 60;
		}

		[[nodiscard]] constexpr size_t minutes() const noexcept {
			return (ns / 60'000'000'000) % 60;
		}

		[[nodiscard]] constexpr size_t hours() const noexcept {
			return (ns / 3'600'000'000'000) % 24;
		}

		[[nodiscard]] constexpr size_t days() const noexcept {
			return (ns / 86'400'000'000'000);
		}


	};

	// A light-weight struct to work with the time
	struct Time final {
		size_t ns;

		[[nodiscard]] static Time now();

		[[nodiscard]] constexpr Duration operator-(Time other) const noexcept {
			return { ns - other.ns };
		}

		[[nodiscard]] constexpr Time operator+(Duration duration) const noexcept {
			return { ns + duration.ns };
		}

		constexpr Time& operator+=(Duration duration) noexcept {
			ns += duration.ns;
			return *this;
		}
	};
}
