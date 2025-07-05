#include "UnitTest.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <Core/Common/DurationToString.hpp>
#include <Core/Common/Timer.hpp>
#include <Core/IO/ConsoleColor.hpp>

namespace core::test {
namespace {
	struct UnitTest {
		char const* name;
		char const* file;
		size_t line;
		UnitTestFunction test;
	};

	constexpr double MAX_ITERATION_TIME_DEVIATION = 0.05;
	constexpr size_t ITERATION_TIME_ADDITION = 100;
	constexpr size_t ITERATION_TIME_WINDOW = 4;
	constexpr size_t ITERATIONS_MAX = 50;

	void printTestsFileHeader(UnitTest const& test) {
		std::cout << io::White << "\n=== Tests file " << test.file << " ===\n";
	}

	void printTestInfo(UnitTest const& test, size_t const testIndex) {
		std::cout << io::White << "Test[" << testIndex << "] " << test.name << ": \n";
	}

	void printTestsResults(size_t const successes, size_t const failures, common::Duration const duration) {
		if (!failures) {
			std::cout << io::Green << "All " << successes << " tests have passed!" << std::endl;
		} else if (!successes) {
			std::cout << io::Red << "All " << failures << " tests have failed." << std::endl;
		} else {
			std::cout << io::Green << successes << " tests have passed!" << std::endl;
			std::cout << io::Red << failures << " tests have failed." << std::endl;
		}
		std::cout << io::Gray << "Overall tests time: " << common::durationToString(duration) << std::endl;
	}

	std::vector<common::Duration> g_testIterationDurations;
	bool g_isSuccess;

	// Returns true if this is the last iteration
	bool onTestIterationEnd(common::Duration const duration) {
		g_testIterationDurations.emplace_back(duration);
		if (g_testIterationDurations.size() < ITERATION_TIME_WINDOW)
			return false;
		if (g_testIterationDurations.size() >= ITERATIONS_MAX) {
			std::cout << io::Yellow << "\tUnstable test iteration time!" << std::endl;
			return true;
		}

		common::Duration minDuration = duration, maxDuration = duration;
		for (size_t i = g_testIterationDurations.size(); i-- > g_testIterationDurations.size() - ITERATION_TIME_WINDOW;) {
			common::Duration iterationDuration = g_testIterationDurations[i];
			if (iterationDuration < minDuration) {
				minDuration = iterationDuration;
			} else if (iterationDuration > maxDuration) {
				maxDuration = iterationDuration;
			}
		}

		size_t const minNs = minDuration.ns + ITERATION_TIME_ADDITION;
		size_t const maxNs = maxDuration.ns + ITERATION_TIME_ADDITION;
		double const diff = static_cast<double>(maxNs) / static_cast<double>(minNs) - 1.0;
		return diff < MAX_ITERATION_TIME_DEVIATION;
	}

	void onTestEnd(common::Duration const duration, bool success) {
		g_isSuccess = true;
		if (success) {
			if (g_testIterationDurations.empty()) {
				std::cout << io::Green << "\tPassed in " << common::durationToString(duration) << std::endl;
			} else {
				std::cout << io::Green << "\tPassed in " << common::durationToString(duration) 
					<< "; average time " << common::durationToString(g_testIterationDurations.back()) << std::endl;
			}
		} else {
			if (g_testIterationDurations.empty()) {
				std::cout << io::Red << "\tFailed in " << common::durationToString(duration) << std::endl;
			} else {
				std::cout << io::Red << "\tFailed in " << common::durationToString(duration)
					<< "; average time " << common::durationToString(g_testIterationDurations.back()) << std::endl;
			}
		}
	}

	bool runUnitTest(UnitTest const& test, size_t const testIndex, bool const shallPrintHeader) {
		if (shallPrintHeader)
			printTestsFileHeader(test);
		printTestInfo(test, testIndex);
		try {
			g_testIterationDurations.clear();
			(*test.test)(UnitTestHelper{ });
			return g_isSuccess;
		} catch (...) {
			std::cout << io::Red << "\tUncaught exception occured during test." << std::endl;
			return false;
		}
	}

	class UnitTestRunner final {
		std::vector<UnitTest> m_unitTests;

	public:
		static inline UnitTestRunner* s_instance = nullptr;

		void addTest(UnitTestNote note, UnitTestFunction unitTest) {
			s_instance = this;
			m_unitTests.emplace_back(note.name, note.file, note.line, unitTest);
		}

		void runAll() {
			std::sort(m_unitTests.begin(), m_unitTests.end(), [](auto const& a, auto const& b) { return a.file < b.file; });

			size_t index = 0;
			size_t successCount = 0;
			char const* prevTestFile = nullptr;
			common::Timer allTestsTimer;
			for (const auto& test : m_unitTests) {
				successCount += runUnitTest(test, index++, prevTestFile != test.file);
				prevTestFile = test.file;
			}

			printTestsResults(successCount, m_unitTests.size() - successCount, allTestsTimer.elapsed());
		}
	};
}

	UnitTestHelper::UnitTestIteratedObject::~UnitTestIteratedObject() {
		if (onTestIterationEnd(timer.elapsed()))
			pIter->isEnd = true;
	}

	UnitTestHelper::UnitTestIterator::value_type UnitTestHelper::UnitTestIterator::operator*() const {
		return UnitTestIteratedObject { this, {} };
	}

	UnitTestHelper::UnitTestIterator::pointer UnitTestHelper::UnitTestIterator::operator->() const {
		return nullptr;
	}

	UnitTestHelper::UnitTestIterator& UnitTestHelper::UnitTestIterator::operator++() {
		return *this;
	}

	UnitTestHelper::UnitTestIterator UnitTestHelper::UnitTestIterator::operator++(int) {
		return *this;
	}

	bool UnitTestHelper::UnitTestIterator::operator==(UnitTestIterator const& other) const noexcept {
		return isEnd == other.isEnd;
	}

	UnitTestHelper::UnitTestHelper(UnitTestHelper&& other) noexcept 
		: m_timer(std::exchange(other.m_timer, common::Timer(common::Time{ 0 })))
		, m_success(other.m_success) {
	}

	UnitTestHelper::~UnitTestHelper() {
		if (m_timer.startTime().ns)
			onTestEnd(m_timer.elapsed(), m_success);
	}

	void UnitTestHelper::assert(bool const condition, char const* const message, std::source_location const location) {
		if (!condition) {
			std::cout << io::Red << "\tTest assertion failed at " << location.line() << ": " << message << std::endl;
			m_success = false;
		}
	}

	UnitTestHelper::UnitTestIterator UnitTestHelper::begin() {
		return { false };
	}

	UnitTestHelper::UnitTestIterator UnitTestHelper::end() {
		return { true };
	}

	void registerUnitTest(UnitTestNote note, UnitTestFunction unitTest) {
		static UnitTestRunner s_unitTestRunner;
		s_unitTestRunner.addTest(std::move(note), unitTest);
	}

	void runAll() {
		if(UnitTestRunner::s_instance)
			UnitTestRunner::s_instance->runAll();
	}
} // namespace core::test
