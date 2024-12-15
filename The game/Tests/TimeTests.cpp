#ifdef _TEST
#include <Core/Test/UnitTest.hpp>
#include <Core/Common/Time.hpp>
#include <Core/Common/DurationToString.hpp>
#include <Core/IO/Logger.hpp>

UNIT_TEST(DurationTest) {
	using namespace core::common;

	constexpr Duration a { 0 };
	constexpr Duration b { 1000 };

	test_assert(a < b);
	test_assert(a <= b);
	test_assert(b > a);
	test_assert(b >= a);
	test_assert(b != a);
	test_assert(a == a);

	test_assert(b - a == b);
};

UNIT_TEST(DurationCastTest) {
	using namespace core::common;

	constexpr Duration d = Duration::Days(9) + Duration::Hours(11) + Duration::Minutes(35) + Duration::Seconds(4) + Duration::Ms(900) + Duration::Us(17);

	test_assert(d.days() == 9);
	test_assert(d.hours() == 11);
	test_assert(d.minutes() == 35);
	test_assert(d.seconds() == 4);
	test_assert(d.ms() == 900);
	test_assert(d.us() == 17);
};

UNIT_TEST(TimeTest) {
	using namespace core::common;

	constexpr Time t0 { 0 };
	constexpr Time t1 { 1'000'000 };
	constexpr Duration d0 { 0 };
	constexpr Duration d1 { 1'000'000 };

	test_assert(t0 == t0);
	test_assert(t0 != t1);
	test_assert(t0 + d0 == t0);
	test_assert(t0 + d1 == t1);
	test_assert(t1 - t0 == d1);
	test_assert(t1 - t1 == d0);
};

UNIT_TEST(DurationToStringTest) {
	using namespace core::common;

	auto asStr = [](Duration const d) { return durationToString(d); };

	test_assert(asStr(Duration{ 0 }) == "00.000'000'000");
	test_assert(asStr(Duration{ 10123456789 }) == "10.123'456'789");
	test_assert(asStr(Duration::Us(31415)) == "00.031'415'000");
	test_assert(asStr(Duration::Ms(2004)) == "02.004'000'000");
	test_assert(asStr(Duration::Seconds(19)) == "19.000'000'000");
	test_assert(asStr(Duration::Minutes(3)) == "03:00.000'000'000");
	test_assert(asStr(Duration::Hours(17)) == "17:00:00.000'000'000");
	test_assert(asStr(Duration::Days(1)) == "1 day, 00:00:00.000'000'000");
	test_assert(asStr(Duration::Days(7)) == "7 days, 00:00:00.000'000'000");
	test_assert(asStr(
		Duration::Days(9) + Duration::Hours(11) + Duration::Minutes(35) + Duration::Seconds(4) + Duration::Ms(900) + Duration::Us(17)
	) == "9 days, 11:35:04.900'017'000");
};

UNIT_TEST(DurationSpeedTest) {
	using namespace core::common;

	Duration d { 0 };
	for (auto _ : test)
		for (size_t i = 0; i < 100'000; ++i)
			NoOpt(d += Duration { i });
};
#endif
