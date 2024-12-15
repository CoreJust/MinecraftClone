#ifdef _TEST
#include <Core/Test/UnitTest.hpp>
#include <Core/Common/Random.hpp>

UNIT_TEST(RandomRangeTest) {
	using namespace core::common;

	Random rnd;
	for (int i = 0; i < 100; ++i) {
		int x = rnd.randi(0, 10);
		test_assert(x >= 0 && x <= 10);
	}
	for (int i = 0; i < 100; ++i) {
		float x = rnd.randf(0.f, 10.f);
		test_assert(x >= 0.f && x < 10.f);
	}
};

UNIT_TEST(RandomSeedTest) {
	using namespace core::common;

	Random rnd;
	for (int i = 0, j = 0; i < 100; ++i, j += i * i) {
		rnd.setSeed(j);
		int x0 = rnd.randi(0, 1024);
		rnd.setSeed(j);
		int x1 = rnd.randi(0, 1024);
		test_assert(x0 == x1);
	}
};
#endif