#ifdef _TEST
#include <Core/Common/Defer.hpp>
#include <Core/Test/UnitTest.hpp>

UNIT_TEST(DeferTest) {
	int i = 0;
	defer { test.assert(++i == 2); };
	defer { test.assert(++i == 1); };
	test.assert(i == 0);
};
#endif