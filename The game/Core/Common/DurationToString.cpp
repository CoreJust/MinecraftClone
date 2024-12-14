#include "DurationToString.hpp"

int countDigits(size_t const x) {
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

void writeDigits(char*& buff, size_t number) noexcept {
	size_t n = countDigits(number);
	buff += n;
	char* ptr = buff;
	while (n-- > 0) {
		*(--ptr) = '0' + number % 10;
		number /= 10;
	}
}

template<size_t N>
void writeDigits(char*& buff, size_t number) noexcept {
	size_t n = N;
	while (n-- > 0) {
		buff[n] = '0' + number % 10;
		number /= 10;
	}
	buff += N;
}

std::string_view core::common::durationToString(Duration duration) noexcept {
	static thread_local char s_data[64];
	char* ptr = s_data;
	bool hasHigherUnit = false;
	if (size_t const days = duration.days()) {
		constexpr static char const* const DAYS_STR[] = { " day, \0\0", " days, \0" };
		writeDigits(ptr, days);
		bool const moreThanOneDay = days > 1;
		*reinterpret_cast<uint64_t*>(ptr) = *reinterpret_cast<uint64_t const*>(DAYS_STR[moreThanOneDay]);
		ptr += 6 + moreThanOneDay;
		hasHigherUnit = true;
	}
	if (size_t const hours = duration.hours(); hours || hasHigherUnit) {
		writeDigits<2>(ptr, hours);
		*(ptr++) = ':';
		hasHigherUnit = true;
	}
	if (size_t const minutes = duration.minutes(); minutes || hasHigherUnit) {
		writeDigits<2>(ptr, minutes);
		*(ptr++) = ':';
		hasHigherUnit = true;
	}
	writeDigits<2>(ptr, duration.seconds());
	*(ptr++) = '.';
	writeDigits<3>(ptr, duration.ms());

	return std::string_view(s_data, ptr - s_data);
}
