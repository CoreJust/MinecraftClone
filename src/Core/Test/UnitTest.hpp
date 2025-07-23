#pragma once
#include <Core/Macro/NameGeneration.hpp>
#define GEN_UNITTEST_NAME(LINE, name) GEN_NAME_(UNITTEST_ ## name, LINE)

#include <source_location>
#include <iterator>
#include <Core/Common/Timer.hpp>
#include <Core/Macro/Attributes.hpp>
#include <Core/Macro/NoOpt.hpp>
#define UNIT_TEST(name) auto GEN_UNITTEST_NAME(__LINE__, name) = ::core::UnitTestNote{ #name, __FILE__, __LINE__ } ^ [](::core::UnitTestHelper test__)
#define CHECK(...) if (!(__VA_ARGS__)) { test__.checkFailure(#__VA_ARGS__); }
#define BENCHMARK(...) for (auto it__ : test__.setBenchmarkingOptions({ __VA_ARGS__ }))
#define WINDOW(n) .window = (n)
#define MAX_ITER(n) .maxIterations = (n)
#define MAX_DEVIATION(n) .maxDeviation = (n)
#define TIME_ADDITION(n) .timeAddition = (n)

namespace core {
	struct UnitTestNote final { 
		char const* const name;
		char const* const file;
		usize line;
	};

	struct UnitTestHelper final {
		struct UnitTestIterator;

		struct BenchmarkingOptions final {
			// Repeats iterations until the difference within last [window] iterations is less than [maxDeviation]
			double maxDeviation = 0.05;
			usize window = 5;
			usize timeAddition = 50; // Added to minimum and maximum time within window to increase stability before evaluation the deviation
			usize maxIterations = 60;
		};

		struct UnitTestIteratedObject final {
			UnitTestIterator const* pIter;
			Timer timer;

			~UnitTestIteratedObject();
		};

		struct UnitTestIterator final {
			using value_type = UnitTestIteratedObject;
			using pointer = const value_type*;
			using reference = const value_type&;
			using difference_type = std::ptrdiff_t;
			using Iterator_category = std::forward_iterator_tag;

			mutable bool isEnd;
			BenchmarkingOptions& options;

			value_type operator*() const;
			pointer operator->() const;
			UnitTestIterator& operator++();
			UnitTestIterator operator++(int);
			bool operator==(UnitTestIterator const& other) const noexcept;
		};

		BenchmarkingOptions m_options { };
		Timer m_timer;
		bool m_success = true;

	public:
		UnitTestHelper() noexcept = default;
		UnitTestHelper(UnitTestHelper&& other) noexcept;
		~UnitTestHelper();

		UnitTestHelper& setBenchmarkingOptions(BenchmarkingOptions options) &noexcept;

		void checkFailure(char const* const message = "", std::source_location const location = std::source_location::current());
		UnitTestIterator begin() &noexcept;
		UnitTestIterator end() &noexcept;
	};

	using UnitTestFunction = void(*)(UnitTestHelper);

	int operator^(UnitTestNote note, auto&& f) {
		registerUnitTest(std::move(note), UnitTestFunction(f));
		return 0;
	}

	void registerUnitTest(UnitTestNote note, UnitTestFunction unitTest);
	void runAll();
} // namespace core
