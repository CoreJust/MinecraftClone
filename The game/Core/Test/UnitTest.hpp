#pragma once
#include <Core/Macro/NameGeneration.hpp>
#define GEN_UNITTEST_NAME(LINE) GEN_NAME_(UNITTEST__, LINE)

#ifndef _TEST
#define UNIT_TEST(name) auto GEN_UNITTEST_NAME(__LINE__) = [](core::test::UnitTestHelper test)
#else
#include <source_location>
#include <iterator>
#include <Core/Common/Timer.hpp>
#define UNIT_TEST(name) auto GEN_UNITTEST_NAME(__LINE__) = core::test::_UnitTestNote{ #name, __FILE__, __LINE__ } ^ [](core::test::UnitTestHelper test)

namespace core::test {
	struct _UnitTestNote final { 
		char const* const name;
		char const* const file;
		size_t line;
	};

	struct UnitTestHelper final {
		struct UnitTestIterator;

		struct UnitTestIteratedObject final {
			UnitTestIterator const* pIter;
			common::Timer timer;

			~UnitTestIteratedObject();
		};

		struct UnitTestIterator final {
			using value_type = UnitTestIteratedObject;
			using pointer = const value_type*;
			using reference = const value_type&;
			using difference_type = std::ptrdiff_t;
			using Iterator_category = std::forward_iterator_tag;

			mutable bool isEnd;

			value_type operator*() const;
			pointer operator->() const;
			UnitTestIterator& operator++();
			UnitTestIterator operator++(int);
			bool operator==(UnitTestIterator const& other) const noexcept;
		};

		common::Timer m_timer;
		bool m_success = true;

	public:
		UnitTestHelper() noexcept = default;
		UnitTestHelper(UnitTestHelper&& other) noexcept;
		~UnitTestHelper();

		void assert(bool const condition, char const* const message = nullptr, std::source_location const location = std::source_location::current());
		UnitTestIterator begin();
		UnitTestIterator end();
	};

	using UnitTestFunction = void(*)(core::test::UnitTestHelper);

	int operator^(_UnitTestNote note, auto&& f) {
		registerUnitTest(std::move(note), UnitTestFunction(f));
		return 0;
	}

	void registerUnitTest(_UnitTestNote note, UnitTestFunction unitTest);
	void runAll();
}
#endif
