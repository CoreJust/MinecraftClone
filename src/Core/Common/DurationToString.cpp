#include "DurationToString.hpp"

namespace core {
namespace {
	int countDigits(u64 const x) {
		if (x >= 10000000000) {
			if (x >= 100000000000000) {
				if (x >= 10000000000000000) {
					if (x >= 100000000000000000) {
						if (x >= 1000000000000000000)
							return 19;
						return 18;
					}
					return 17;
				}
				if (x >= 1000000000000000)
					return 16;
				return 15;
			}
			if (x >= 1000000000000) {
				if (x >= 10000000000000)
					return 14;
				return 13;
			}
			if (x >= 100000000000)
				return 12;
			return 11;
		}
		if (x >= 100000) {
			if (x >= 10000000) {
				if (x >= 100000000) {
					if (x >= 1000000000)
						return 10;
					return 9;
				}
				return 8;
			}
			if (x >= 1000000)
				return 7;
			return 6;
		}
		if (x >= 100) {
			if (x >= 1000) {
				if (x >= 10000)
					return 5;
				return 4;
			}
			return 3;
		}
		if (x >= 10)
			return 2;
		return 1;
	}

	void writeDigits(char*& buff, u64 number) noexcept {
		int n = countDigits(number);
		buff += n;
		char* ptr = buff;
		while (n-- > 0) {
			*(--ptr) = '0' + number % 10;
			number /= 10;
		}
	}

	template<usize N>
	void writeDigits(char*& buff, u64 number) noexcept {
		usize n = N;
		while (n-- > 0) {
			buff[n] = '0' + number % 10;
			number /= 10;
		}
		buff += N;
	}
} // namespace

	std::string_view durationToString(Duration duration) noexcept {
		static thread_local char s_data[64];
		char* ptr = s_data;
		bool hasHigherUnit = false;
		if (usize const days = duration.days()) {
			constexpr static char const* const DAYS_STR[] = { " day, \0\0", " days, \0" };
			writeDigits(ptr, days);
			bool const moreThanOneDay = days > 1;
			*reinterpret_cast<u64*>(ptr) = *reinterpret_cast<u64 const*>(DAYS_STR[moreThanOneDay]);
			ptr += 6 + moreThanOneDay;
			hasHigherUnit = true;
		}
		if (usize const hours = duration.hours(); hours || hasHigherUnit) {
			writeDigits<2>(ptr, hours);
			*(ptr++) = ':';
			hasHigherUnit = true;
		}
		if (usize const minutes = duration.minutes(); minutes || hasHigherUnit) {
			writeDigits<2>(ptr, minutes);
			*(ptr++) = ':';
			hasHigherUnit = true;
		}
		writeDigits<2>(ptr, duration.seconds());
		*(ptr++) = '.';
		writeDigits<3>(ptr, duration.ms());
		*(ptr++) = '\'';
		writeDigits<3>(ptr, duration.us());
		*(ptr++) = '\'';
		writeDigits<3>(ptr, duration.ns % 1'000);

		return std::string_view(s_data, static_cast<usize>(ptr - s_data));
	}
} // namespace core
