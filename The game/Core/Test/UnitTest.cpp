#ifdef _TEST
#include "UnitTest.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <Core/Common/DurationToString.hpp>
#include <Core/Common/Timer.hpp>
#include <Core/IO/ConsoleColor.hpp>

namespace {
	struct UnitTest {
		char const* name;
		char const* file;
		size_t line;
		core::test::UnitTestFunction test;
	};

	constexpr double MAX_ITERATION_TIME_DEVIATION = 0.05;
	constexpr size_t ITERATION_TIME_ADDITION = 100;
	constexpr size_t ITERATION_TIME_WINDOW = 3;

	void printTestsFileHeader(UnitTest const& test) {
		std::cout << core::io::White << "\n=== Tests file " << test.file << " ===\n";
	}

	void printTestInfo(UnitTest const& test, size_t const testIndex) {
		std::cout << core::io::White << "Test[" << testIndex << "] " << test.name << ": ";
	}

	void printTestsResults(size_t const successes, size_t const failures, core::common::Duration const duration) {
		if (!failures) {
			std::cout << core::io::Green << "All " << successes << " tests have passed!" << std::endl;
		} else if (!successes) {
			std::cout << core::io::Red << "All " << failures << " tests have failed." << std::endl;
		} else {
			std::cout << core::io::Green << successes << " tests have passed!" << std::endl;
			std::cout << core::io::Red << failures << " tests have failed." << std::endl;
		}
		std::cout << core::io::Gray << "Overall tests time: " << core::common::durationToString(duration) << std::endl;
	}

	std::vector<core::common::Duration> g_testIterationDurations;
	bool g_isSuccess;

	// Returns true if this is the last iteration
	bool onTestIterationEnd(core::common::Duration const duration) {
		g_testIterationDurations.emplace_back(duration);
		if (g_testIterationDurations.size() < ITERATION_TIME_WINDOW)
			return false;

		core::common::Duration minDuration = duration, maxDuration = duration;
		for (size_t i = g_testIterationDurations.size(); i-- >= g_testIterationDurations.size() - ITERATION_TIME_WINDOW;) {
			core::common::Duration iterationDuration = g_testIterationDurations[i];
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

	void onTestEnd(core::common::Duration const duration, bool success) {
		g_isSuccess = true;
		if (success) {
			if (g_testIterationDurations.empty()) {
				std::cout << core::io::Green << "Passed in " << core::common::durationToString(duration) << std::endl;
			} else {
				std::cout << core::io::Green << "Passed in " << core::common::durationToString(duration) 
					<< "; average time " << core::common::durationToString(g_testIterationDurations.back()) << std::endl;
			}
		} else {
			if (g_testIterationDurations.empty()) {
				std::cout << core::io::Red << "Failed in " << core::common::durationToString(duration) << std::endl;
			} else {
				std::cout << core::io::Red << "Failed in " << core::common::durationToString(duration)
					<< "; average time " << core::common::durationToString(g_testIterationDurations.back()) << std::endl;
			}
		}
	}

	bool runUnitTest(UnitTest const& test, size_t const testIndex, bool const shallPrintHeader) {
		if (shallPrintHeader)
			printTestsFileHeader(test);
		printTestInfo(test, testIndex);
		try {
			g_testIterationDurations.clear();
			(*test.test)(core::test::UnitTestHelper{ });
			return g_isSuccess;
		} catch (...) {
			std::cout << core::io::Red << "\tUncaught exception occured during test." << std::endl;
			return false;
		}
	}

	class UnitTestRunner final {
		std::vector<UnitTest> m_unitTests;

	public:
		static inline UnitTestRunner* s_instance = nullptr;

		void addTest(core::test::_UnitTestNote note, core::test::UnitTestFunction unitTest) {
			s_instance = this;
			m_unitTests.emplace_back(note.name, note.file, note.line, unitTest);
		}

		void runAll() {
			std::sort(m_unitTests.begin(), m_unitTests.end(), [](auto const& a, auto const& b) { return a.file < b.file; });

			size_t index = 0;
			size_t successCount = 0;
			char const* prevTestFile = nullptr;
			core::common::Timer allTestsTimer;
			for (const auto& test : m_unitTests) {
				successCount += runUnitTest(test, index++, prevTestFile != test.file);
				prevTestFile = test.file;
			}

			printTestsResults(successCount, m_unitTests.size() - successCount, allTestsTimer.elapsed());
		}
	};
}

core::test::UnitTestHelper::UnitTestIteratedObject::~UnitTestIteratedObject() {
	if (onTestIterationEnd(timer.elapsed()))
		pIter->isEnd = true;
}

core::test::UnitTestHelper::UnitTestIterator::value_type core::test::UnitTestHelper::UnitTestIterator::operator*() const {
	return UnitTestIteratedObject { this };
}

core::test::UnitTestHelper::UnitTestIterator::pointer core::test::UnitTestHelper::UnitTestIterator::operator->() const {
	return nullptr;
}

core::test::UnitTestHelper::UnitTestIterator& core::test::UnitTestHelper::UnitTestIterator::operator++() {
	return *this;
}

core::test::UnitTestHelper::UnitTestIterator core::test::UnitTestHelper::UnitTestIterator::operator++(int) {
	return *this;
}

bool core::test::UnitTestHelper::UnitTestIterator::operator==(UnitTestIterator const& other) const noexcept {
	return isEnd == other.isEnd;
}

core::test::UnitTestHelper::UnitTestHelper(UnitTestHelper&& other) noexcept 
	: m_timer(std::exchange(other.m_timer, core::common::Timer(core::common::Time{ 0 })))
	, m_success(other.m_success) {
}

core::test::UnitTestHelper::~UnitTestHelper() {
	if (m_timer.startTime().ns)
		onTestEnd(m_timer.elapsed(), m_success);
}

void core::test::UnitTestHelper::assert(bool const condition, char const* const message, std::source_location const location) {
	if (!condition) {
		std::cout << io::Red << "\tTest assertion failed at " << location.line() << ": " << message << std::endl;
		m_success = false;
	}
}

core::test::UnitTestHelper::UnitTestIterator core::test::UnitTestHelper::begin() {
	return { false };
}

core::test::UnitTestHelper::UnitTestIterator core::test::UnitTestHelper::end() {
	return { true };
}

void core::test::registerUnitTest(_UnitTestNote note, UnitTestFunction unitTest) {
	static UnitTestRunner s_unitTestRunner;
	s_unitTestRunner.addTest(std::move(note), unitTest);
}

void core::test::runAll() {
	if(UnitTestRunner::s_instance)
		UnitTestRunner::s_instance->runAll();
}
#endif
