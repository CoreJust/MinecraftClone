#include <Core/Test/UnitTest.hpp>
#include "../Time.hpp"
#include "../DurationToString.hpp"

using namespace core;

UNIT_TEST(DurationTest) {
	constexpr Duration a { 0 };
	constexpr Duration b { 1000 };

	CHECK(a < b);
	CHECK(a <= b);
	CHECK(b > a);
	CHECK(b >= a);
	CHECK(b != a);
	CHECK(a == a);

	CHECK(b - a == b);
};

UNIT_TEST(DurationCastTest) {
	constexpr Duration d = Duration::Days(9) + Duration::Hours(11) + Duration::Minutes(35) + Duration::Seconds(4) + Duration::Ms(900) + Duration::Us(17);

	CHECK(d.days() == 9);
	CHECK(d.hours() == 11);
	CHECK(d.minutes() == 35);
	CHECK(d.seconds() == 4);
	CHECK(d.ms() == 900);
	CHECK(d.us() == 17);
};

UNIT_TEST(TimeTest) {
	constexpr Time t0 { 0 };
	constexpr Time t1 { 1'000'000 };
	constexpr Duration d0 { 0 };
	constexpr Duration d1 { 1'000'000 };

	CHECK(t0 == t0);
	CHECK(t0 != t1);
	CHECK(t0 + d0 == t0);
	CHECK(t0 + d1 == t1);
	CHECK(t1 - t0 == d1);
	CHECK(t1 - t1 == d0);
};

UNIT_TEST(DurationToStringTest) {
	auto asStr = [](Duration const d) { return durationToString(d); };

	CHECK(asStr(Duration{ 0 }) == "00.000'000'000");
	CHECK(asStr(Duration{ 10123456789 }) == "10.123'456'789");
	CHECK(asStr(Duration::Us(31415)) == "00.031'415'000");
	CHECK(asStr(Duration::Ms(2004)) == "02.004'000'000");
	CHECK(asStr(Duration::Seconds(19)) == "19.000'000'000");
	CHECK(asStr(Duration::Minutes(3)) == "03:00.000'000'000");
	CHECK(asStr(Duration::Hours(17)) == "17:00:00.000'000'000");
	CHECK(asStr(Duration::Days(1)) == "1 day, 00:00:00.000'000'000");
	CHECK(asStr(Duration::Days(7)) == "7 days, 00:00:00.000'000'000");
	CHECK(asStr(
		Duration::Days(9) + Duration::Hours(11) + Duration::Minutes(35) + Duration::Seconds(4) + Duration::Ms(900) + Duration::Us(17)
	) == "9 days, 11:35:04.900'017'000");
};

UNIT_TEST(DurationSpeedTest) {
	Duration d { 0 };
	for (auto _ : test)
		for (usize i = 0; i < 100'000; ++i)
			NO_OPT(d += Duration { i });
};
