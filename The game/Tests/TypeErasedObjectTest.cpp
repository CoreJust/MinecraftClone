#ifdef _TEST
#include <functional>
#include <Core/Test/UnitTest.hpp>
#include <Core/Memory/TypeErasedObject.hpp>

struct SmallTestStruct {
	char c;
	int a;

	SmallTestStruct(char c, int a) : a(a), c(c) { }
};

struct LargeTestStruct {
	std::function<void(LargeTestStruct*)> func;
	size_t someData[2];

	LargeTestStruct(std::function<void(LargeTestStruct*)> f, size_t const(&d)[2]) 
		: func(std::move(f)), someData { d[0], d[1] } { }
	~LargeTestStruct() {
		func(this);
	}
};

UNIT_TEST(TypeErasedObjectTrivialTest) {
	using IntAccessor = core::memory::TypeErasedObject<int>;

	void* erasedObject = IntAccessor::make(10);
	test_assert(IntAccessor::get(erasedObject) == 10);

	IntAccessor::get(erasedObject) = 12;
	test_assert(IntAccessor::get(erasedObject) == 12);
};

UNIT_TEST(TypeErasedObjectSmallTest) {
	using Accessor = core::memory::TypeErasedObject<SmallTestStruct>;

	void* erasedObject = Accessor::make('!', -13);
	test_assert(Accessor::get(erasedObject).c == '!');
	test_assert(Accessor::get(erasedObject).a == -13);

	Accessor::get(erasedObject).a += 15;
	test_assert(Accessor::get(erasedObject).a == 2);
};

UNIT_TEST(TypeErasedObjectLargeTest) {
	using Accessor = core::memory::TypeErasedObject<LargeTestStruct>;

	size_t arr[2] = { 199, 123245 };
	bool wasObjectDestroyed = false;

	void* erasedObject = Accessor::make([&](LargeTestStruct* object) {
		test_assert(object->someData[0] == 199 && object->someData[1] == 123245);
		wasObjectDestroyed = true;
	}, arr);
	Accessor::destroy(erasedObject);
	test_assert(wasObjectDestroyed);
};
#endif