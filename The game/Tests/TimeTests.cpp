#include <Core/Common/Time.hpp>
#include <Core/Test/UnitTest.hpp>

#ifdef _TEST
UNIT_TEST(DurationTest) {
	using namespace core::common;

	Duration a { 0 };
	Duration b { 1000 };

	test.assert(a < b);
	test.assert(a <= b);
	test.assert(b > a);
	test.assert(b >= a);
	test.assert(b != a);
	test.assert(a == a);

	test.assert(b - a == b);
};
#endif
